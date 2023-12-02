//
// Aspia Project
// Copyright (C) 2016-2023 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "client/client.h"

#include "base/logging.h"
#include "base/task_runner.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "client/status_window_proxy.h"

#if defined(OS_MAC)
#include "base/mac/app_nap_blocker.h"
#endif // defined(OS_MAC)

namespace client {

//--------------------------------------------------------------------------------------------------
Client::Client(std::shared_ptr<base::TaskRunner> io_task_runner)
    : io_task_runner_(std::move(io_task_runner))
{
    LOG(LS_INFO) << "Ctor";
    DCHECK(io_task_runner_);

#if defined(OS_MAC)
    base::addAppNapBlock();
#endif // defined(OS_MAC)
}

//--------------------------------------------------------------------------------------------------
Client::~Client()
{
    LOG(LS_INFO) << "Dtor";
    DCHECK(io_task_runner_->belongsToCurrentThread());
    stop();

#if defined(OS_MAC)
    base::releaseAppNapBlock();
#endif // defined(OS_MAC)
}

//--------------------------------------------------------------------------------------------------
void Client::start(const Config& config)
{
    DCHECK(io_task_runner_->belongsToCurrentThread());
    DCHECK(status_window_proxy_);

    if (state_ != State::CREATED)
    {
        LOG(LS_ERROR) << "Client already started before";
        return;
    }

    config_ = config;
    state_ = State::STARTED;

    if (base::isHostId(config_.address_or_id))
    {
        LOG(LS_INFO) << "Starting RELAY connection";

        if (!config_.router_config.has_value())
        {
            LOG(LS_FATAL) << "No router config. Continuation is impossible";
            return;
        }

        router_controller_ =
            std::make_unique<RouterController>(*config_.router_config, io_task_runner_);

        if (!reconnect_in_progress_)
        {
            // Show the status window.
            status_window_proxy_->onStarted();
        }

        status_window_proxy_->onRouterConnecting(
            config_.router_config->address, config_.router_config->port);
        router_controller_->connectTo(
            base::stringToHostId(config_.address_or_id), reconnect_in_progress_, this);
    }
    else
    {
        LOG(LS_INFO) << "Starting DIRECT connection";

        if (!reconnect_in_progress_)
        {
            // Show the status window.
            status_window_proxy_->onStarted();
        }

        // Create a network channel for messaging.
        channel_ = std::make_unique<base::TcpChannel>();

        // Set the listener for the network channel.
        channel_->setListener(this);

        // Now connect to the host.
        status_window_proxy_->onHostConnecting(config_.address_or_id, config_.port);
        channel_->connect(config_.address_or_id, config_.port);
    }
}

//--------------------------------------------------------------------------------------------------
void Client::stop()
{
    DCHECK(io_task_runner_->belongsToCurrentThread());

    if (state_ != State::STOPPPED)
    {
        LOG(LS_INFO) << "Stopping client...";
        state_ = State::STOPPPED;

        router_controller_.reset();
        authenticator_.reset();
        channel_.reset();
        timeout_timer_.reset();

        auto_reconnect_ = false;
        reconnect_in_progress_ = false;

        status_window_proxy_->onStopped();

        LOG(LS_INFO) << "Client stopped";
    }
    else
    {
        LOG(LS_ERROR) << "Client already stopped";
    }
}

//--------------------------------------------------------------------------------------------------
void Client::setStatusWindow(std::shared_ptr<StatusWindowProxy> status_window_proxy)
{
    LOG(LS_INFO) << "Status window installed";
    status_window_proxy_ = std::move(status_window_proxy);
}

//--------------------------------------------------------------------------------------------------
bool Client::isAutoReconnect()
{
    return auto_reconnect_;
}

//--------------------------------------------------------------------------------------------------
void Client::setAutoReconnect(bool enable)
{
    LOG(LS_INFO) << "Auto reconnect changed: " << enable;
    auto_reconnect_ = enable;
}

//--------------------------------------------------------------------------------------------------
std::u16string Client::computerName() const
{
    return config_.computer_name;
}

//--------------------------------------------------------------------------------------------------
proto::SessionType Client::sessionType() const
{
    return config_.session_type;
}

//--------------------------------------------------------------------------------------------------
void Client::sendMessage(uint8_t channel_id, const google::protobuf::MessageLite& message)
{
    if (!channel_)
    {
        LOG(LS_ERROR) << "sendMessage called but channel not initialized";
        return;
    }

    channel_->send(channel_id, base::serialize(message));
}

//--------------------------------------------------------------------------------------------------
int64_t Client::totalRx() const
{
    if (!channel_)
    {
        LOG(LS_ERROR) << "totalRx called but channel not initialized";
        return 0;
    }

    return channel_->totalRx();
}

//--------------------------------------------------------------------------------------------------
int64_t Client::totalTx() const
{
    if (!channel_)
    {
        LOG(LS_ERROR) << "totalTx called but channel not initialized";
        return 0;
    }

    return channel_->totalTx();
}

//--------------------------------------------------------------------------------------------------
int Client::speedRx()
{
    if (!channel_)
    {
        LOG(LS_ERROR) << "speedRx called but channel not initialized";
        return 0;
    }

    return channel_->speedRx();
}

//--------------------------------------------------------------------------------------------------
int Client::speedTx()
{
    if (!channel_)
    {
        LOG(LS_ERROR) << "speedTx called but channel not initialized";
        return 0;
    }

    return channel_->speedTx();
}

//--------------------------------------------------------------------------------------------------
void Client::onTcpConnected()
{
    LOG(LS_INFO) << "Connection established";
    startAuthentication();
}

//--------------------------------------------------------------------------------------------------
void Client::onTcpDisconnected(base::NetworkChannel::ErrorCode error_code)
{
    LOG(LS_INFO) << "Connection terminated: " << base::NetworkChannel::errorToString(error_code);

    // Show an error to the user.
    status_window_proxy_->onHostDisconnected(error_code);

    if (isAutoReconnect())
    {
        LOG(LS_INFO) << "Reconnect to host enabled";
        reconnect_in_progress_ = true;

        if (!timeout_timer_)
        {
            timeout_timer_ = std::make_unique<base::WaitableTimer>(
                base::WaitableTimer::Type::SINGLE_SHOT, io_task_runner_);
            timeout_timer_->start(std::chrono::minutes(5), [this]()
            {
                LOG(LS_INFO) << "Reconnect timeout";
                status_window_proxy_->onWaitForHostTimeout();

                reconnect_in_progress_ = false;
                setAutoReconnect(false);

                if (channel_)
                {
                    channel_->setListener(nullptr);
                    channel_.reset();
                }

                router_controller_.reset();
            });
        }

        // Delete old channel.
        if (channel_)
        {
            channel_->setListener(nullptr);
            io_task_runner_->deleteSoon(std::move(channel_));
        }

        if (base::isHostId(config_.address_or_id))
        {
            // If you are using an ID connection, then start the connection immediately. The Router
            // will notify you when the Host comes online again.
            state_ = State::CREATED;
            start(config_);
        }
        else
        {
            reconnect_timer_ = std::make_unique<base::WaitableTimer>(
                base::WaitableTimer::Type::SINGLE_SHOT, io_task_runner_);
            reconnect_timer_->start(std::chrono::seconds(5), [this]()
            {
                LOG(LS_INFO) << "Reconnecting to host";
                state_ = State::CREATED;
                start(config_);
            });
        }
    }
}

//--------------------------------------------------------------------------------------------------
void Client::onTcpMessageReceived(uint8_t channel_id, const base::ByteArray& buffer)
{
    if (channel_id == proto::HOST_CHANNEL_ID_SESSION)
    {
        onSessionMessageReceived(channel_id, buffer);
    }
    else if (channel_id == proto::HOST_CHANNEL_ID_SERVICE)
    {
        // TODO
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled incoming message from channel: " << channel_id;
    }
}

//--------------------------------------------------------------------------------------------------
void Client::onTcpMessageWritten(uint8_t channel_id, size_t pending)
{
    if (channel_id == proto::HOST_CHANNEL_ID_SESSION)
    {
        onSessionMessageWritten(channel_id, pending);
    }
    else if (channel_id == proto::HOST_CHANNEL_ID_SERVICE)
    {
        // TODO
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled outgoing message from channel: " << channel_id;
    }
}

//--------------------------------------------------------------------------------------------------
void Client::onRouterConnected(const std::u16string& address, uint16_t port)
{
    LOG(LS_INFO) << "Router connected (address=" << address << " port=" << port << ")";
    status_window_proxy_->onRouterConnected(address, port);
}

//--------------------------------------------------------------------------------------------------
void Client::onHostAwaiting()
{
    LOG(LS_INFO) << "Host awaiting";
    status_window_proxy_->onWaitForHost();
}

//--------------------------------------------------------------------------------------------------
void Client::onHostConnected(std::unique_ptr<base::TcpChannel> channel)
{
    LOG(LS_INFO) << "Host connected";
    DCHECK(channel);

    channel_ = std::move(channel);
    channel_->setListener(this);

    startAuthentication();

    // Router controller is no longer needed.
    io_task_runner_->deleteSoon(std::move(router_controller_));
}

//--------------------------------------------------------------------------------------------------
void Client::onErrorOccurred(const RouterController::Error& error)
{
    status_window_proxy_->onRouterError(error);
}

//--------------------------------------------------------------------------------------------------
void Client::startAuthentication()
{
    LOG(LS_INFO) << "Start authentication for '" << config_.username << "'";

    reconnect_in_progress_ = false;
    reconnect_timer_.reset();
    timeout_timer_.reset();

    static const size_t kReadBufferSize = 2 * 1024 * 1024; // 2 Mb.

    channel_->setReadBufferSize(kReadBufferSize);
    channel_->setNoDelay(true);
    channel_->setKeepAlive(true);

    authenticator_ = std::make_unique<base::ClientAuthenticator>(io_task_runner_);

    authenticator_->setIdentify(proto::IDENTIFY_SRP);
    authenticator_->setUserName(config_.username);
    authenticator_->setPassword(config_.password);
    authenticator_->setSessionType(static_cast<uint32_t>(config_.session_type));

    authenticator_->start(std::move(channel_),
                          [this](base::ClientAuthenticator::ErrorCode error_code)
    {
        if (error_code == base::ClientAuthenticator::ErrorCode::SUCCESS)
        {
            LOG(LS_INFO) << "Successful authentication";

            // The authenticator takes the listener on itself, we return the receipt of
            // notifications.
            channel_ = authenticator_->takeChannel();
            channel_->setListener(this);

            const base::Version& host_version = authenticator_->peerVersion();
            if (host_version >= base::Version::kVersion_2_6_0)
            {
                LOG(LS_INFO) << "Using channel id support";
                channel_->setChannelIdSupport(true);
            }

            const base::Version& client_version = base::Version::kVersion_CurrentFull;
            if (host_version > client_version)
            {
                LOG(LS_ERROR) << "Version mismatch (host: " << host_version.toString()
                              << " client: " << client_version.toString();
                status_window_proxy_->onVersionMismatch(host_version, client_version);
            }
            else
            {
                status_window_proxy_->onHostConnected(config_.address_or_id, config_.port);

                // Signal that everything is ready to start the session (connection established,
                // authentication passed).
                onSessionStarted(host_version);

                // Now the session will receive incoming messages.
                channel_->resume();
            }
        }
        else
        {
            LOG(LS_INFO) << "Failed authentication: "
                         << base::ClientAuthenticator::errorToString(error_code);
            status_window_proxy_->onAccessDenied(error_code);
        }

        // Authenticator is no longer needed.
        io_task_runner_->deleteSoon(std::move(authenticator_));
    });
}

} // namespace client
