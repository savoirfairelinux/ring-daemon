/*
 *  Copyright (C) 2017 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "peer_connection.h"

#include "data_transfer.h"
#include "account.h"
#include "string_utils.h"
#include "channel.h"
#include "turn_transport.h"
#include "security/tls_session.h"

#include <algorithm>
#include <future>
#include <map>
#include <atomic>
#include <stdexcept>
#include <istream>
#include <ostream>
#include <unistd.h>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#endif

#include <sys/time.h>

namespace ring {

using lock = std::lock_guard<std::mutex>;

static constexpr std::size_t IO_BUFFER_SIZE {3000}; ///< Size of char buffer used by IO operations

//==============================================================================

class TlsTurnEndpoint::Impl
{
public:
    static constexpr auto TLS_TIMEOUT = std::chrono::seconds(20);

    Impl(ConnectedTurnTransport& tr,
         dht::crypto::TrustList& trust_list) : turn {tr}, trustList {trust_list} {}

    ~Impl();

    // TLS callbacks
    int verifyCertificate(gnutls_session_t);
    void onTlsStateChange(tls::TlsSessionState);
    void onTlsRxData(std::vector<uint8_t>&&);
    void onTlsCertificatesUpdate(const gnutls_datum_t*, const gnutls_datum_t*, unsigned int);

    std::unique_ptr<tls::TlsSession> tls;
    ConnectedTurnTransport& turn;
    dht::crypto::TrustList& trustList;
    dht::crypto::Certificate peerCertificate;
    std::promise<bool> connected;
};

// Declaration at namespace scope is necessary (until C++17)
constexpr std::chrono::seconds TlsTurnEndpoint::Impl::TLS_TIMEOUT;

TlsTurnEndpoint::Impl::~Impl()
{
    if (peerCertificate)
        trustList.remove(peerCertificate);
}

int
TlsTurnEndpoint::Impl::verifyCertificate(gnutls_session_t session)
{
    // Support only x509 format
    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    // Store verification status
    unsigned int status = 0;
    auto ret = gnutls_certificate_verify_peers2(session, &status);
    if (ret < 0 or (status & GNUTLS_CERT_SIGNATURE_FAILURE) != 0) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    unsigned int cert_list_size = 0;
    auto cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
    if (cert_list == nullptr) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    // Check if peer certificate is inside our list of permited certificate
    std::vector<std::pair<uint8_t*, uint8_t*>> crt_data;
    crt_data.reserve(cert_list_size);
    for (unsigned i=0; i<cert_list_size; i++)
        crt_data.emplace_back(cert_list[i].data, cert_list[i].data + cert_list[i].size);
    auto crt = dht::crypto::Certificate {crt_data};
    auto verify_result = trustList.verify(crt);
    if (!verify_result) {
        RING_ERR() << "[TLS-TURN] Peer certificate verification failed: " << verify_result;
        return verify_result.result;
    }

    // Store valid peer certificate for trust list removal during dtor
    peerCertificate = std::move(crt);

    // notify GnuTLS to continue handshake normally
    return GNUTLS_E_SUCCESS;
}

void
TlsTurnEndpoint::Impl::onTlsStateChange(tls::TlsSessionState state)
{
    if (state == tls::TlsSessionState::ESTABLISHED) {
        connected.set_value(true);
    }
}

void
TlsTurnEndpoint::Impl::onTlsRxData(UNUSED std::vector<uint8_t>&& buf)
{
    RING_ERR() << "[TLS-TURN] rx " << buf.size() << " (but not implemented)";
}

void
TlsTurnEndpoint::Impl::onTlsCertificatesUpdate(UNUSED const gnutls_datum_t* local_raw,
                                               UNUSED const gnutls_datum_t* remote_raw,
                                               UNUSED unsigned int remote_count)
{}

TlsTurnEndpoint::TlsTurnEndpoint(ConnectedTurnTransport& turn_ep,
                                 const Identity& local_identity,
                                 const std::shared_future<tls::DhParams>& dh_params,
                                 dht::crypto::TrustList& trust_list)
    : pimpl_ { std::make_unique<Impl>(turn_ep, trust_list) }
{
    // Add TLS over TURN
    tls::TlsSession::TlsSessionCallbacks tls_cbs = {
        /*.onStateChange = */[this](tls::TlsSessionState state){ pimpl_->onTlsStateChange(state); },
        /*.onRxData = */[this](std::vector<uint8_t>&& buf){ pimpl_->onTlsRxData(std::move(buf)); },
        /*.onCertificatesUpdate = */[this](const gnutls_datum_t* l, const gnutls_datum_t* r,
                                           unsigned int n){ pimpl_->onTlsCertificatesUpdate(l, r, n); },
        /*.verifyCertificate = */[this](gnutls_session_t session){ return pimpl_->verifyCertificate(session); }
    };
    tls::TlsParams tls_param = {
        /*.ca_list = */     "",
        /*.peer_ca = */     nullptr,
        /*.cert = */        local_identity.second,
        /*.cert_key = */    local_identity.first,
        /*.dh_params = */   dh_params,
        /*.timeout = */     Impl::TLS_TIMEOUT,
        /*.cert_check = */  nullptr,
    };
    pimpl_->tls = std::make_unique<tls::TlsSession>(turn_ep, tls_param, tls_cbs);
}

TlsTurnEndpoint::~TlsTurnEndpoint() = default;

bool
TlsTurnEndpoint::isInitiator() const
{
    return pimpl_->tls->isInitiator();
}

void
TlsTurnEndpoint::connect()
{
    pimpl_->tls->connect();
}

int
TlsTurnEndpoint::maxPayload() const
{
    return pimpl_->tls->maxPayload();
}

std::size_t
TlsTurnEndpoint::read(ValueType* buf, std::size_t len, std::error_code& ec)
{
    return pimpl_->tls->read(buf, len, ec);
}

std::size_t
TlsTurnEndpoint::write(const ValueType* buf, std::size_t len, std::error_code& ec)
{
    return pimpl_->tls->write(buf, len, ec);
}

//==============================================================================

TcpSocketEndpoint::TcpSocketEndpoint(const IpAddr& addr)
    : addr_ {addr}
    , sock_ {::socket(addr.getFamily(), SOCK_STREAM, 0)}
{
    if (sock_ < 0)
        std::system_error(errno, std::generic_category());
    auto bound = ip_utils::getAnyHostAddr(addr.getFamily());
    if (::bind(sock_, bound, bound.getLength()) < 0)
        std::system_error(errno, std::generic_category());
}

TcpSocketEndpoint::~TcpSocketEndpoint()
{
    ::close(sock_);
}

void
TcpSocketEndpoint::connect()
{
    // Blocking method
    if (::connect(sock_, addr_, addr_.getLength()) < 0)
        throw std::system_error(errno, std::generic_category());
}

bool
TcpSocketEndpoint::waitForData(unsigned ms_timeout) const
{
    struct timeval tv;
    tv.tv_sec = ms_timeout / 1000;
    tv.tv_usec = (ms_timeout % 1000) * 1000;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock_, &read_fds);

    while (::select(sock_ + 1, &read_fds, nullptr, nullptr, &tv) >= 0) {
        if (FD_ISSET(sock_, &read_fds))
            return true;
    }

    return false;
}

std::size_t
TcpSocketEndpoint::read(ValueType* buf, std::size_t len, std::error_code& ec)
{
    // NOTE: recv buf args is a void* on POSIX compliant system, but it's a char* on mingw
    auto res = ::recv(sock_, reinterpret_cast<char*>(buf), len, 0);
    ec.assign(errno, std::generic_category());
    return (res >= 0) ? res : 0;
}

std::size_t
TcpSocketEndpoint::write(const ValueType* buf, std::size_t len, std::error_code& ec)
{
    // NOTE: recv buf args is a void* on POSIX compliant system, but it's a char* on mingw
    auto res = ::send(sock_, reinterpret_cast<const char*>(buf), len, 0);
    ec.assign(errno, std::generic_category());
    return (res >= 0) ? res : 0;
}

//==============================================================================

class TlsSocketEndpoint::Impl
{
public:
    static constexpr auto TLS_TIMEOUT = std::chrono::seconds(20);

    Impl(TcpSocketEndpoint& ep, const dht::crypto::Certificate& peer_cert)
        : tr {ep}, peerCertificate {peer_cert} {}

    // TLS callbacks
    int verifyCertificate(gnutls_session_t);
    void onTlsStateChange(tls::TlsSessionState);
    void onTlsRxData(std::vector<uint8_t>&&);
    void onTlsCertificatesUpdate(const gnutls_datum_t*, const gnutls_datum_t*, unsigned int);

    std::unique_ptr<tls::TlsSession> tls;
    TcpSocketEndpoint& tr;
    const dht::crypto::Certificate& peerCertificate;
};

// Declaration at namespace scope is necessary (until C++17)
constexpr std::chrono::seconds TlsSocketEndpoint::Impl::TLS_TIMEOUT;

int
TlsSocketEndpoint::Impl::verifyCertificate(gnutls_session_t session)
{
    // Support only x509 format
    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    // Store verification status
    unsigned int status = 0;
    auto ret = gnutls_certificate_verify_peers2(session, &status);
    if (ret < 0 or (status & GNUTLS_CERT_SIGNATURE_FAILURE) != 0) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    unsigned int cert_list_size = 0;
    auto cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
    if (cert_list == nullptr) {
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    // Check if peer certificate is equal to the expected one
    std::vector<std::pair<uint8_t*, uint8_t*>> crt_data;
    crt_data.reserve(cert_list_size);
    for (unsigned i=0; i<cert_list_size; i++)
        crt_data.emplace_back(cert_list[i].data, cert_list[i].data + cert_list[i].size);
    auto crt = dht::crypto::Certificate {crt_data};
    if (crt.getId() != peerCertificate.getId()) {
        RING_ERR() << "[TLS-SOCKET] Unexpected peer certificate";
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    // notify GnuTLS to continue handshake normally
    return GNUTLS_E_SUCCESS;
}

void
TlsSocketEndpoint::Impl::onTlsStateChange(UNUSED tls::TlsSessionState state)
{}

void
TlsSocketEndpoint::Impl::onTlsRxData(UNUSED std::vector<uint8_t>&& buf)
{}

void
TlsSocketEndpoint::Impl::onTlsCertificatesUpdate(UNUSED const gnutls_datum_t* local_raw,
                                                UNUSED const gnutls_datum_t* remote_raw,
                                                UNUSED unsigned int remote_count)
{}

TlsSocketEndpoint::TlsSocketEndpoint(TcpSocketEndpoint& tr,
                                     const Identity& local_identity,
                                     const std::shared_future<tls::DhParams>& dh_params,
                                     const dht::crypto::Certificate& peer_cert)
    : pimpl_ { std::make_unique<Impl>(tr, peer_cert) }
{
    // Add TLS over TURN
    tls::TlsSession::TlsSessionCallbacks tls_cbs = {
        /*.onStateChange = */[this](tls::TlsSessionState state){ pimpl_->onTlsStateChange(state); },
        /*.onRxData = */[this](std::vector<uint8_t>&& buf){ pimpl_->onTlsRxData(std::move(buf)); },
        /*.onCertificatesUpdate = */[this](const gnutls_datum_t* l, const gnutls_datum_t* r,
                                           unsigned int n){ pimpl_->onTlsCertificatesUpdate(l, r, n); },
        /*.verifyCertificate = */[this](gnutls_session_t session){ return pimpl_->verifyCertificate(session); }
    };
    tls::TlsParams tls_param = {
        /*.ca_list = */     "",
        /*.peer_ca = */     nullptr,
        /*.cert = */        local_identity.second,
        /*.cert_key = */    local_identity.first,
        /*.dh_params = */   dh_params,
        /*.timeout = */     Impl::TLS_TIMEOUT,
        /*.cert_check = */  nullptr,
    };
    pimpl_->tls = std::make_unique<tls::TlsSession>(tr, tls_param, tls_cbs);
}

TlsSocketEndpoint::~TlsSocketEndpoint() = default;

std::size_t
TlsSocketEndpoint::read(ValueType* buf, std::size_t len, std::error_code& ec)
{
    return pimpl_->tls->read(buf, len, ec);
}

std::size_t
TlsSocketEndpoint::write(const ValueType* buf, std::size_t len, std::error_code& ec)
{
    return pimpl_->tls->write(buf, len, ec);
}

void
TlsSocketEndpoint::connect()
{
    pimpl_->tls->connect();
}

//==============================================================================

// following namespace prevents an ODR violation with definitions in p2p.cpp
namespace
{

enum class CtrlMsgType
{
    STOP,
    ATTACH_INPUT,
    ATTACH_OUTPUT,
};

struct CtrlMsg
{
    virtual CtrlMsgType type() const = 0;
    virtual ~CtrlMsg() = default;
};

struct StopCtrlMsg final : CtrlMsg
{
    explicit StopCtrlMsg() {}
    CtrlMsgType type() const override { return CtrlMsgType::STOP; }
};

struct AttachInputCtrlMsg final : CtrlMsg
{
    explicit AttachInputCtrlMsg(const std::shared_ptr<Stream>& stream)
        : stream {stream} {}
    CtrlMsgType type() const override { return CtrlMsgType::ATTACH_INPUT; }
    const std::shared_ptr<Stream> stream;
};

struct AttachOutputCtrlMsg final : CtrlMsg
{
    explicit AttachOutputCtrlMsg(const std::shared_ptr<Stream>& stream)
        : stream {stream} {}
    CtrlMsgType type() const override { return CtrlMsgType::ATTACH_OUTPUT; }
    const std::shared_ptr<Stream> stream;
};

} // namespace <anonymous>

//==============================================================================

class PeerConnection::PeerConnectionImpl
{
public:
    PeerConnectionImpl(Account& account, const std::string& peer_uri,
                       std::unique_ptr<SocketType> endpoint)
        : account {account}
        , peer_uri {peer_uri}
        , endpoint_ {std::move(endpoint)}
        , eventLoopFut_ {std::async(std::launch::async, [this]{ eventLoop();})} {}

    ~PeerConnectionImpl() {
        ctrlChannel << std::make_unique<StopCtrlMsg>();
    }

    const Account& account;
    const std::string peer_uri;
    Channel<std::unique_ptr<CtrlMsg>> ctrlChannel;

private:
    std::unique_ptr<SocketType> endpoint_;
    std::map<DRing::DataTransferId, std::shared_ptr<Stream>> inputs_;
    std::map<DRing::DataTransferId, std::shared_ptr<Stream>> outputs_;
    std::future<void> eventLoopFut_;

    void eventLoop();

    template <typename L, typename C>
    void handle_stream_list(L& stream_list, const C& callable) {
        if (stream_list.empty())
            return;
        const auto& item = std::begin(stream_list);
        auto& stream = item->second;
        try {
            if (callable(stream))
                return;
            RING_DBG() << "EOF on stream #" << stream->getId();
        } catch (const std::system_error& e) {
            RING_WARN() << "Stream #" << stream->getId()
                        << " IO failed with code = " << e.code();
        } catch (const std::exception& e) {
            RING_ERR() << "Unexpected exception during IO with stream #"
                       << stream->getId()
                       << ": " << e.what();
        }
        stream->close();
        stream_list.erase(item);
    }
};

void
PeerConnection::PeerConnectionImpl::eventLoop()
{
    RING_DBG() << "[CNX] Peer connection to " << peer_uri << " ready";
    while (true) {
        // Process ctrl orders first
        while (true) {
            std::unique_ptr<CtrlMsg> msg;
            if (outputs_.empty() and inputs_.empty()) {
                ctrlChannel >> msg;
            } else if (!ctrlChannel.empty()) {
                msg = ctrlChannel.receive();
            } else
                break;

            switch (msg->type()) {
                case CtrlMsgType::ATTACH_INPUT:
                {
                    auto& input_msg = static_cast<AttachInputCtrlMsg&>(*msg);
                    auto id = input_msg.stream->getId();
                    inputs_.emplace(id, std::move(input_msg.stream));
                }
                break;

                case CtrlMsgType::ATTACH_OUTPUT:
                {
                    auto& output_msg = static_cast<AttachOutputCtrlMsg&>(*msg);
                    auto id = output_msg.stream->getId();
                    outputs_.emplace(id, std::move(output_msg.stream));
                }
                break;

                case CtrlMsgType::STOP:
                    endpoint_.reset();
                    inputs_.clear();
                    outputs_.clear();
                    return;

                default: RING_ERR("BUG: got unhandled control msg!");  break;
            }
        }

        // Then handles IO streams
        std::vector<uint8_t> buf(IO_BUFFER_SIZE);
        std::error_code ec;
        handle_stream_list(inputs_, [&](auto& stream){
                if (!stream->read(buf))
                    return false;
                auto size = endpoint_->write(buf, ec);
                if (!ec)
                    return true;
                if (!size)
                    return false;
                throw std::system_error(ec);
            });
        handle_stream_list(outputs_, [&](auto& stream){
                endpoint_->read(buf, ec);
                return buf.size() != 0 and stream->write(buf);
            });
    }
}

//==============================================================================

PeerConnection::PeerConnection(Account& account, const std::string& peer_uri,
                               std::unique_ptr<GenericSocket<uint8_t>> endpoint)
    : pimpl_(std::make_unique<PeerConnectionImpl>(account, peer_uri, std::move(endpoint)))
{}

PeerConnection::~PeerConnection()
{}

void
PeerConnection::attachInputStream(const std::shared_ptr<Stream>& stream)
{
    pimpl_->ctrlChannel << std::make_unique<AttachInputCtrlMsg>(stream);
}

void
PeerConnection::attachOutputStream(const std::shared_ptr<Stream>& stream)
{
    pimpl_->ctrlChannel << std::make_unique<AttachOutputCtrlMsg>(stream);
}

} // namespace ring