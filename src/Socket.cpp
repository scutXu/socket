#include <assert.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <string.h>
#include "Socket.h"

#define BLOCK_SIZE 1024

Socket::Socket()
{
	m_fd = -1;
	m_state = CLOSED;
}

Socket::Socket(int domain, int type, int protocol)
{
	m_fd = -1;
	m_state = CLOSED;
	open(domain, type, protocol);
}

Socket::Socket(Socket && that)
{
	m_fd = that.m_fd;
	m_state = that.m_state;

	that.m_fd = -1;
	that.m_state = CLOSED;
}

Socket::~Socket()
{
	close();
}

int Socket::getFD()
{
	return m_fd;
}

Socket::State Socket::getState()
{
	return m_state;
}

void Socket::setNonBlocking()
{
	int flag = fcntl(m_fd, F_GETFL, 0);
	if (flag != -1) {
		fcntl(m_fd, F_SETFL, flag | O_NONBLOCK);
	}
}

error_code Socket::open(int domain, int type, int protocol)
{
	assert(m_state == CLOSED);
	m_fd = socket(domain, type, protocol);
	if (m_fd >= 0) {
		m_state = OPENED;
        return error_code(0, std::generic_category());
	}
	assert(errno != 0);
    return error_code(errno, std::generic_category());
}

error_code Socket::bind(const char * ipAddress, uint16_t port)
{
	EndPoint ep(ipAddress, port);
	return bind(ep);
}


error_code Socket::bind(EndPoint & ep)
{
	assert(m_state == OPENED);
	int status = ::bind(m_fd, &ep.addr, sizeof(ep));
	if (status == 0) {
		m_state = BOUND;
		return error_code(0, std::generic_category());
	}
	assert(errno != 0);
	return error_code(errno, std::generic_category());
}

error_code Socket::listen(int backlog)
{
	assert(m_state == BOUND);
	int status = ::listen(m_fd, backlog);
	if (status == 0) {
		m_state = LISTENING;
		return error_code(0, std::generic_category());
	}
	assert(errno != 0);
	return error_code(errno, std::generic_category());
}

void Socket::connect(const char * ipAddress, uint16_t port, ConnectCallback cb)
{
	EndPoint ep(ipAddress, port);
	connect(ep, cb);
}

void Socket::connect(EndPoint & ep, ConnectCallback cb)
{
	assert(m_state == OPENED || m_state == BOUND);
	int status = ::connect(m_fd, &ep.addr, sizeof(ep));
	if (status == 0) {
		m_state = CONNECTED;
		cb(error_code(0, std::generic_category()));
	}
	else {
		if (errno == EINPROGRESS || errno == EINTR) {
			m_state = CONNECTING;
			m_connectRequest = cb;
		}
		else {
			int e = errno;
			close();
			assert(e != 0);
			cb(error_code(errno, std::generic_category()));
		}
	}
}

void Socket::accept(AcceptCallback cb)
{
	assert(m_state == LISTENING);
	m_acceptRequests.push(cb);
}

void Socket::read(int size, ReadCallback cb)
{
	//todo:socket may be closed when error occured
	assert(m_state == CONNECTING || m_state == CONNECTED);
	ReadRequest rq;
	rq.cb = cb;
	rq.size = size;
	m_readRequests.push(rq);
}

void Socket::readUntil(char delim, ReadCallback cb)
{
	assert(m_state == CONNECTING || m_state == CONNECTED);
	ReadRequest rq;
	rq.cb = cb;
	rq.size = -1;
	rq.delim = delim;
	m_readRequests.push(rq);
}

void Socket::write(void * data, int size, WriteCallback cb)
{
	assert(m_state == CONNECTING || m_state == CONNECTED);

	size_t oldSize = m_writeBuffer.size();
	m_writeBuffer.resize(oldSize + size);
	memcpy(&m_writeBuffer[oldSize], data, size);

	WriteRequest wq;
	wq.cb = cb;
	wq.size = size;
	m_writeRequests.push(wq);
}

error_code Socket::close()
{
	if (m_fd >= 0) {
		int status = ::close(m_fd);
		m_fd = -1;
		m_state = CLOSED;
		if (status == 0) {
			return error_code(0, std::generic_category());
		}
		assert(errno != 0);
		return error_code(errno, std::generic_category());
	}
	return error_code(0, std::generic_category());
}

bool Socket::waitToRead()
{
	return m_state == CONNECTING
		|| (m_state == CONNECTED)
		|| (m_state == LISTENING && !m_acceptRequests.empty());
}

bool Socket::waitToWrite()
{
	return m_state == CONNECTED && !m_writeBuffer.empty();
}

void Socket::doRead()
{
	if (m_state == LISTENING) {
		if (!m_acceptRequests.empty()) {
			int fd = ::accept(m_fd, nullptr, nullptr);
			if (fd >= 0) {
				Socket s;
				s.m_fd = fd;
				s.m_state = CONNECTED;
				AcceptCallback cb = m_acceptRequests.front();
				m_acceptRequests.pop();
				cb(std::move(s), error_code(0, std::generic_category()));
			}
			else {
				if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ECONNABORTED) {
					//just wait for next connection arrived
				}
				else {
					AcceptCallback cb = m_acceptRequests.front();
					m_acceptRequests.pop();
					cb(Socket(), error_code(errno, std::generic_category()));
				}
			}
			
		}
	}
	else if (m_state == CONNECTING) {
		//check if connection successed
		int status = ::read(m_fd, nullptr, 0);
		if (status == 0) {
			m_state = CONNECTED;
			m_connectRequest(error_code(0, std::generic_category()));
		}
		else {
            int e = errno;
            close();
            assert(e != 0);
            m_connectRequest(error_code(errno, std::generic_category()));
		}
	}
	else if (m_state == CONNECTED) {
		ssize_t bytesRead = 0;
		ssize_t totalBytesRead = 0;
		do {
			size_t originSize = m_readBuffer.size();
			m_readBuffer.resize(originSize + BLOCK_SIZE);
			bytesRead = ::read(m_fd, &m_readBuffer[0], BLOCK_SIZE);
			m_readBuffer.resize(originSize + std::max(0L, bytesRead));
			totalBytesRead += std::max(0L, bytesRead);
		} while (bytesRead > 0);
		if (totalBytesRead > 0) {
			while (!m_readRequests.empty()) {
				ReadRequest rq = m_readRequests.front();
				if (rq.size > 0) {
					if (m_readBuffer.size() > rq.size) {
						rq.cb(&m_readBuffer[0], rq.size, error_code(0, std::generic_category()));
						m_readRequests.pop();
						m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + rq.size);
					}
				}
				else {
					for (ssize_t i = 0; i < m_readBuffer.size(); ++i) {
						if (m_readBuffer[i] == rq.delim) {
							rq.cb(&m_readBuffer[0], i + 1, error_code(0, std::generic_category()));
							m_readRequests.pop();
							m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + i + 1);
						}
					}
				}
			}
		}
		if (bytesRead < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				//todo:return error and do some clear
			}
		}
		else {

		}
	}
	else {
		assert(false);
	}
}
