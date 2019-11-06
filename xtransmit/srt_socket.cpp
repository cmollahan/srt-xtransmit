#include <thread>
#include <iostream>
#include <iterator> // std::ostream_iterator

#include "srt_socket.hpp"

#include "verbose.hpp"
#include "socketoptions.hpp"
#include "apputil.hpp"

using namespace std;
using namespace xtransmit;
using shared_socket = shared_ptr<srt::socket>;

srt::socket::socket(const UriParser &src_uri)
	: m_host(src_uri.host())
	, m_port(src_uri.portno())
	, m_options(src_uri.parameters())
{
	m_bind_socket = srt_create_socket();
	if (m_bind_socket == SRT_INVALID_SOCK)
		throw socket_exception(srt_getlasterror_str());

	if (m_options.count("blocking"))
	{
		m_blocking_mode = !false_names.count(m_options.at("blocking"));
		m_options.erase("blocking");
	}

	if (!m_blocking_mode)
	{
		m_epoll_connect = srt_epoll_create();
		if (m_epoll_connect == -1)
			throw socket_exception(srt_getlasterror_str());

		int modes = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_connect, m_bind_socket, &modes))
			throw socket_exception(srt_getlasterror_str());

		m_epoll_io = srt_epoll_create();
		modes      = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_io, m_bind_socket, &modes))
			throw socket_exception(srt_getlasterror_str());
	}

	if (SRT_SUCCESS != configure_pre(m_bind_socket))
		throw socket_exception(srt_getlasterror_str());
}

srt::socket::socket(const int sock, bool blocking)
	: m_bind_socket(sock)
	, m_blocking_mode(blocking)
{
	if (!m_blocking_mode)
	{
		m_epoll_io = srt_epoll_create();
		int modes  = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_io, m_bind_socket, &modes))
			throw socket_exception(srt_getlasterror_str());
	}
}

srt::socket::~socket()
{
	if (!m_blocking_mode)
	{
		Verb() << "Releasing epolls for socket " << m_bind_socket;
		if (m_epoll_connect != -1)
			srt_epoll_release(m_epoll_connect);
		srt_epoll_release(m_epoll_io);
	}
	Verb() << "Closing socket " << m_bind_socket;
	srt_close(m_bind_socket);
}

void srt::socket::listen()
{
	int         num_clients = 2;
	sockaddr_in sa;

	try
	{
		sa = CreateAddrInet(m_host, m_port);
	}
	catch (const std::invalid_argument &e)
	{
		raise_exception("create_addr_inet", e.what());
	}

	sockaddr *psa = (sockaddr *)&sa;
	Verb() << "Binding a server on " << m_host << ":" << m_port << " ..." << VerbNoEOL;
	int res = srt_bind(m_bind_socket, psa, sizeof sa);
	if (res == SRT_ERROR)
	{
		srt_close(m_bind_socket);
		raise_exception("srt_bind", UDT::getlasterror());
	}

	res = srt_listen(m_bind_socket, num_clients);
	if (res == SRT_ERROR)
	{
		srt_close(m_bind_socket);
		raise_exception("srt_listen", UDT::getlasterror());
	}

	Verb() << " listening.";
	res = configure_post(m_bind_socket);
	if (res == SRT_ERROR)
		raise_exception("configure_post", UDT::getlasterror());
}

shared_socket srt::socket::accept()
{
	sockaddr_in scl;
	int         sclen = sizeof scl;

	// Wait for REAL connected state if nonblocking mode
	if (!m_blocking_mode)
	{
		Verb() << "[ASYNC] " << VerbNoEOL;

		// Socket readiness for connection is checked by polling on WRITE allowed sockets.

		constexpr int timeout_ms = -1;
		int           len        = 2;
		SRTSOCKET     ready[2];
		if (srt_epoll_wait(m_epoll_connect, 0, 0, ready, &len, timeout_ms, 0, 0, 0, 0) == -1)
		{
			// if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
			//	continue;

			raise_exception("srt_epoll_wait", UDT::getlasterror());
		}

		Verb() << "[EPOLL: " << len << " sockets] " << VerbNoEOL;
	}

	SRTSOCKET sock = srt_accept(m_bind_socket, (sockaddr *)&scl, &sclen);
	if (sock == SRT_INVALID_SOCK)
	{
		raise_exception("srt_accept", UDT::getlasterror());
	}

	// we do one client connection at a time,
	// so close the listener.
	// srt_close(m_bindsock);
	// m_bindsock = SRT_INVALID_SOCK;

	Verb() << " connected.";

	int res = configure_post(sock);
	if (res == SRT_ERROR)
		raise_exception("configure_post", UDT::getlasterror());

	return make_shared<socket>(sock, m_blocking_mode);
}

void srt::socket::raise_exception(const string &&place, UDT::ERRORINFO &udt_error)
{
	const int    udt_result = udt_error.getErrorCode();
	const string message    = udt_error.getErrorMessage();
	Verb() << place << " ERROR #" << udt_result << ": " << message;

	udt_error.clear();
	throw socket_exception("error at " + place + ": " + message);
}

void srt::socket::raise_exception(const string &&place, const string &&reason)
{
	Verb() << "raise exception at " << place << ": " << reason;

	throw socket_exception("Error at " + place + ": " + reason);
}

shared_socket srt::socket::connect()
{
	sockaddr_in sa;
	try
	{
		sa = CreateAddrInet(m_host, m_port);
	}
	catch (const std::invalid_argument &e)
	{
		raise_exception("create_addr_inet", e.what());
	}


	sockaddr *psa = (sockaddr *)&sa;
	Verb() << "Connecting to " << m_host << ":" << m_port << (m_blocking_mode ? " (SYNC)" : " (ASYNC)");

	{
		const int res = srt_connect(m_bind_socket, psa, sizeof sa);
		if (res == SRT_ERROR)
		{
			srt_close(m_bind_socket);
			raise_exception("srt_connect", UDT::getlasterror());
		}
	}

	// Wait for REAL connected state if nonblocking mode
	if (!m_blocking_mode)
	{
		// Socket readiness for connection is checked by polling on WRITE allowed sockets.
		int       len = 2;
		SRTSOCKET ready[2];
		if (srt_epoll_wait(m_epoll_connect, 0, 0, ready, &len, -1, 0, 0, 0, 0) != -1)
		{
			const SRT_SOCKSTATUS state = srt_getsockstate(m_bind_socket);
			if (state != SRTS_CONNECTED)
				raise_exception("srt::socket::connect", "connection failed, socket state " + to_string(state));
		}
		else
		{
			raise_exception("srt_epoll_wait", UDT::getlasterror());
		}
	}

	Verb() << " connected.";
	{
		const int res = configure_post(m_bind_socket);
		if (res == SRT_ERROR)
			raise_exception("configure_post", UDT::getlasterror());
	}

	return shared_from_this();
}

std::future<shared_socket> srt::socket::async_connect()
{
	auto self = shared_from_this();

	return async(std::launch::async, [self]() { return self->connect(); });
}

std::future<shared_socket> srt::socket::async_accept()
{
	listen();

	auto self = shared_from_this();
	return async(std::launch::async, [self]() { return self->accept(); });
}

std::future<shared_socket> srt::socket::async_read(std::vector<char> &buffer) { return std::future<shared_socket>(); }

int srt::socket::configure_pre(SRTSOCKET sock)
{
	int maybe  = m_blocking_mode ? 1 : 0;
	int result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &maybe, sizeof maybe);
	if (result == -1)
		return result;

	// host is only checked for emptiness and depending on that the connection mode is selected.
	// Here we are not exactly interested with that information.
	std::vector<string> failures;

	// NOTE: here host = "", so the 'connmode' will be returned as LISTENER always,
	// but it doesn't matter here. We don't use 'connmode' for anything else than
	// checking for failures.
	SocketOption::Mode conmode = SrtConfigurePre(sock, m_host, m_options, &failures);

	if (conmode == SocketOption::FAILURE)
	{
		if (Verbose::on)
		{
			Verb() << "WARNING: failed to set options: ";
			copy(failures.begin(), failures.end(), ostream_iterator<string>(*Verbose::cverb, ", "));
			Verb();
		}

		return SRT_ERROR;
	}

	m_mode = static_cast<connection_mode>(conmode);

	return SRT_SUCCESS;
}

int srt::socket::configure_post(SRTSOCKET sock)
{
	int is_blocking = m_blocking_mode ? 1 : 0;

	int result = srt_setsockopt(sock, 0, SRTO_SNDSYN, &is_blocking, sizeof is_blocking);
	if (result == -1)
		return result;
	result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &is_blocking, sizeof is_blocking);
	if (result == -1)
		return result;

	// host is only checked for emptiness and depending on that the connection mode is selected.
	// Here we are not exactly interested with that information.
	vector<string> failures;

	SrtConfigurePost(sock, m_options, &failures);

	if (!failures.empty())
	{
		if (Verbose::on)
		{
			Verb() << "WARNING: failed to set options: ";
			copy(failures.begin(), failures.end(), ostream_iterator<string>(*Verbose::cverb, ", "));
			Verb();
		}
	}

	return 0;
}

size_t srt::socket::read(const mutable_buffer &buffer, int timeout_ms)
{
	if (!m_blocking_mode)
	{
		int ready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int len      = 2;

		const int epoll_res = srt_epoll_wait(m_epoll_io, ready, &len, nullptr, nullptr, timeout_ms, 0, 0, 0, 0);
		if (epoll_res == SRT_ERROR)
		{
			if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
				return 0;

			raise_exception("socket::read::epoll", UDT::getlasterror());
		}
	}

	const int res = srt_recvmsg2(m_bind_socket, static_cast<char *>(buffer.data()), (int)buffer.size(), nullptr);
	if (SRT_ERROR == res)
		raise_exception("socket::read::recv", UDT::getlasterror());

	return static_cast<size_t>(res);
}

int srt::socket::write(const const_buffer &buffer, int timeout_ms)
{
	stringstream ss;
	if (!m_blocking_mode)
	{
		int ready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int len      = 2;
		int rready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int rlen      = 2;
		// TODO: check error fds
		const int res = srt_epoll_wait(m_epoll_io, rready, &rlen, ready, &len, timeout_ms, 0, 0, 0, 0);
		if (res == SRT_ERROR)
			raise_exception("socket::write::epoll", UDT::getlasterror());

		ss << "srt::socket::write: srt_epoll_wait res " << res << " rlen " << rlen << " wlen " << len << " wsocket " << ready[0];
		//Verb() << "srt::socket::write: srt_epoll_wait set len " << len << " socket " << ready[0];
	}

	const int res = srt_sendmsg2(m_bind_socket, static_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), nullptr);
	if (res == SRT_ERROR)
	{
		size_t blocks, bytes;
		srt_getsndbuffer(m_bind_socket, &blocks, &bytes);
		int sndbuf = 0;
		int optlen = sizeof sndbuf;
		srt_getsockopt(m_bind_socket, 0, SRTO_SNDBUF, &sndbuf, &optlen);
		ss << " SND Buffer: " << bytes << " / " << sndbuf << " bytes";
		ss << " (" << sndbuf - bytes << " bytes remaining)";
		ss << "trying to write " << buffer.size() << "bytes";
		raise_exception("socket::write::send", srt_getlasterror_str() + ss.str());
		//	raise_exception("socket::write::send", UDT::getlasterror());
	}

	return res;
}

srt::socket::connection_mode srt::socket::mode() const { return m_mode; }

int srt::socket::statistics(SRT_TRACEBSTATS &stats) { return srt_bstats(m_bind_socket, &stats, true); }

const string srt::socket::statistics_csv(bool print_header)
{
	SRT_TRACEBSTATS stats;
	if (SRT_ERROR == srt_bstats(m_bind_socket, &stats, true))
		raise_exception("socket::statistics", UDT::getlasterror());

	std::ostringstream output;

	if (print_header)
	{
		output << "Time,SocketID,pktFlowWindow,pktCongestionWindow,pktFlightSize,";
		output << "msRTT,mbpsBandwidth,mbpsMaxBW,pktSent,pktSndLoss,pktSndDrop,";
		output << "pktRetrans,byteSent,byteAvailSndBuf,byteSndDrop,mbpsSendRate,usPktSndPeriod,";
		output << "pktRecv,pktRcvLoss,pktRcvDrop,pktRcvRetrans,pktRcvBelated,";
		output << "byteRecv,byteAvailRcvBuf,byteRcvLoss,byteRcvDrop,mbpsRecvRate,msRcvTsbPdDelay";
		output << endl;
	}

	output << stats.msTimeStamp << ",";
	output << m_bind_socket << ",";
	output << stats.pktFlowWindow << ",";
	output << stats.pktCongestionWindow << ",";
	output << stats.pktFlightSize << ",";

	output << stats.msRTT << ",";
	output << stats.mbpsBandwidth << ",";
	output << stats.mbpsMaxBW << ",";
	output << stats.pktSent << ",";
	output << stats.pktSndLoss << ",";
	output << stats.pktSndDrop << ",";

	output << stats.pktRetrans << ",";
	output << stats.byteSent << ",";
	output << stats.byteAvailSndBuf << ",";
	output << stats.byteSndDrop << ",";
	output << stats.mbpsSendRate << ",";
	output << stats.usPktSndPeriod << ",";

	output << stats.pktRecv << ",";
	output << stats.pktRcvLoss << ",";
	output << stats.pktRcvDrop << ",";
	output << stats.pktRcvRetrans << ",";
	output << stats.pktRcvBelated << ",";

	output << stats.byteRecv << ",";
	output << stats.byteAvailRcvBuf << ",";
	output << stats.byteRcvLoss << ",";
	output << stats.byteRcvDrop << ",";
	output << stats.mbpsRecvRate << ",";
	output << stats.msRcvTsbPdDelay;

	output << endl;

	return output.str();
}
