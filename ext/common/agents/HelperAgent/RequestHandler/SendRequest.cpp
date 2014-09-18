/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

// This file is included inside the RequestHandler class.

private:

void
sendHeaderToApp(Client *client, Request *req) {
	TRACE_POINT();
	SKC_TRACE(client, 2, "Sending headers to application with " <<
		req->session->getProtocol() << " protocol");
	req->state = Request::SENDING_HEADER_TO_APP;

	/**
	 * HTTP does not formally support half-closing, and Node.js treats a
	 * half-close as a full close, so we only half-close session sockets, not
	 * HTTP sockets.
	 */
	if (req->session->getProtocol() == "session") {
		UPDATE_TRACE_POINT();
		req->halfCloseAppConnection = true;
		sendHeaderToAppWithSessionProtocol(client, req);
	} else {
		UPDATE_TRACE_POINT();
		req->halfCloseAppConnection = false;
		sendHeaderToAppWithHttpProtocol(client, req);
	}

	UPDATE_TRACE_POINT();
	if (!req->ended()) {
		if (!req->appSink.ended()) {
			if (!req->appSink.passedThreshold()) {
				UPDATE_TRACE_POINT();
				sendBodyToApp(client, req);
				req->appSource.startReading();
			} else {
				UPDATE_TRACE_POINT();
				SKC_TRACE(client, 3, "Waiting for appSink buffers to be "
					"flushed before sending body to application");
				req->appSink.setBuffersFlushedCallback(sendBodyToAppWhenBuffersFlushed);
				req->appSource.startReading();
			}
		} else {
			// req->appSink.feed() encountered an error while writing to the
			// application socket. But we don't care about that; we just care that
			// ForwardResponse.cpp will now forward the response data and end the
			// request.
			UPDATE_TRACE_POINT();
			req->state = Request::WAITING_FOR_APP_OUTPUT;
			req->appSource.startReading();
		}
	}
}

struct SessionProtocolWorkingState {
	StaticString path;
	StaticString queryString;
	StaticString methodStr;
	StaticString serverName;
	StaticString serverPort;
	const LString *remoteAddr;
	const LString *remotePort;
	const LString *remoteUser;
	const LString *contentLength;
	const LString *contentType;
	bool hasBaseURI;
};

void
sendHeaderToAppWithSessionProtocol(Client *client, Request *req) {
	TRACE_POINT();
	SessionProtocolWorkingState state;
	unsigned int bufferSize = determineHeaderSizeForSessionProtocol(req,
		state);
	MemoryKit::mbuf_pool &mbuf_pool = getContext()->mbuf_pool;
	const unsigned int MBUF_MAX_SIZE = mbuf_pool.mbuf_block_chunk_size -
		mbuf_pool.mbuf_block_offset;
	bool ok;

	if (bufferSize <= MBUF_MAX_SIZE) {
		MemoryKit::mbuf buffer(MemoryKit::mbuf_get(&mbuf_pool));
		bufferSize = MBUF_MAX_SIZE;

		ok = constructHeaderForSessionProtocol(req, buffer.start,
			bufferSize, state);
		assert(ok);
		buffer = MemoryKit::mbuf(buffer, 0, bufferSize);
		SKC_TRACE(client, 3, "Header data: \"" << cEscapeString(
			StaticString(buffer.start, bufferSize)) << "\"");
		req->appSink.feed(buffer);
	} else {
		char *buffer = (char *) psg_pnalloc(req->pool, bufferSize);

		ok = constructHeaderForSessionProtocol(req, buffer,
			bufferSize, state);
		assert(ok);
		SKC_TRACE(client, 3, "Header data: \"" << cEscapeString(
			StaticString(buffer, bufferSize)) << "\"");
		req->appSink.feed(buffer, bufferSize);
	}

	(void) ok; // Shut up compiler warning
}

static void
sendBodyToAppWhenBuffersFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdSinkChannel *channel =
		reinterpret_cast<FileBufferedFdSinkChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(
		getServerFromClient(client));
	SKC_LOG_EVENT_FROM_STATIC(self, RequestHandler, client, "sendBodyToAppWhenBuffersFlushed");

	req->appSink.setBuffersFlushedCallback(NULL);
	self->sendBodyToApp(client, req);
}

unsigned int
determineHeaderSizeForSessionProtocol(Request *req,
	SessionProtocolWorkingState &state)
{
	unsigned int dataSize = sizeof(boost::uint32_t);

	state.path        = req->getPathWithoutQueryString();
	state.hasBaseURI  = req->options.baseURI != P_STATIC_STRING("/");
	if (state.hasBaseURI) {
		state.path = state.path.substr(req->options.baseURI.size());
		if (state.path.empty()) {
			state.path = P_STATIC_STRING("/");
		}
	}
	state.queryString = req->getQueryString();
	state.methodStr   = StaticString(http_method_str(req->method));
	state.remoteAddr  = req->secureHeaders.lookup(REMOTE_ADDR);
	state.remotePort  = req->secureHeaders.lookup(REMOTE_PORT);
	state.remoteUser  = req->secureHeaders.lookup(REMOTE_USER);
	state.contentLength = req->headers.lookup(HTTP_CONTENT_LENGTH);
	state.contentType   = req->headers.lookup(HTTP_CONTENT_TYPE);

	dataSize += sizeof("REQUEST_URI");
	dataSize += req->path.size + 1;

	dataSize += sizeof("PATH_INFO");
	dataSize += state.path.size() + 1;

	dataSize += sizeof("SCRIPT_NAME");
	if (state.hasBaseURI) {
		dataSize += req->options.baseURI.size();
	} else {
		dataSize += sizeof("");
	}

	dataSize += sizeof("QUERY_STRING");
	dataSize += state.queryString.size() + 1;

	dataSize += sizeof("REQUEST_METHOD");
	dataSize += state.methodStr.size() + 1;

	if (req->host != NULL) {
		const LString *host = psg_lstr_make_contiguous(req->host, req->pool);
		const char *sep = (const char *) memchr(host->start->data, ':', host->size);
		if (sep != NULL) {
			state.serverName = StaticString(host->start->data, sep - host->start->data);
			state.serverPort = StaticString(sep + 1,
				host->start->data + host->size - sep - 1);
		} else {
			state.serverName = StaticString(host->start->data, host->size);
			state.serverPort = P_STATIC_STRING("80");
		}
	} else {
		state.serverName = defaultServerName;
		state.serverPort = defaultServerPort;
	}

	dataSize += sizeof("SERVER_NAME");
	dataSize += state.serverName.size() + 1;

	dataSize += sizeof("SERVER_PORT");
	dataSize += state.serverPort.size() + 1;

	dataSize += sizeof("SERVER_SOFTWARE");
	dataSize += serverSoftware.size() + 1;

	dataSize += sizeof("SERVER_PROTOCOL");
	dataSize += sizeof("HTTP/1.1");

	dataSize += sizeof("REMOTE_ADDR");
	if (state.remoteAddr != NULL) {
		dataSize += state.remoteAddr->size + 1;
	} else {
		dataSize += sizeof("127.0.0.1");
	}

	dataSize += sizeof("REMOTE_PORT");
	if (state.remotePort != NULL) {
		dataSize += state.remotePort->size + 1;
	} else {
		dataSize += sizeof("0");
	}

	if (state.remoteUser != NULL) {
		dataSize += sizeof("REMOTE_USER");
		dataSize += state.remoteUser->size + 1;
	}

	if (state.contentLength != NULL) {
		dataSize += sizeof("CONTENT_LENGTH");
		dataSize += state.contentLength->size + 1;
	}

	if (state.contentLength != NULL) {
		dataSize += sizeof("CONTENT_TYPE");
		dataSize += state.contentType->size + 1;
	}

	dataSize += sizeof("PASSENGER_CONNECT_PASSWORD");
	dataSize += req->session->getGroupSecret().size() + 1;

	if (req->https) {
		dataSize += sizeof("HTTPS");
		dataSize += sizeof("on");
	}

	if (req->options.analytics) {
		dataSize += sizeof("PASSENGER_TXN_ID");
		dataSize += req->options.transaction->getTxnId().size() + 1;
	}

	ServerKit::HeaderTable::Iterator it(req->headers);
	while (*it != NULL) {
		dataSize += sizeof("HTTP_") - 1 + it->header->key.size + 1;
		dataSize += it->header->val.size + 1;
		it.next();
	}

	return dataSize + 1;
}

bool
constructHeaderForSessionProtocol(Request *req, char * restrict buffer, unsigned int &size,
	const SessionProtocolWorkingState &state)
{
	char *pos = buffer;
	const char *end = buffer + size;

	pos += sizeof(boost::uint32_t);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REQUEST_URI"));
	pos = appendData(pos, end, req->path.start->data, req->path.size);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PATH_INFO"));
	pos = appendData(pos, end, state.path.data(), state.path.size());
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SCRIPT_NAME"));
	if (state.hasBaseURI) {
		pos = appendData(pos, end, req->options.baseURI);
		pos = appendData(pos, end, "", 1);
	} else {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL(""));
	}

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("QUERY_STRING"));
	pos = appendData(pos, end, state.queryString.data(), state.queryString.size());
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REQUEST_METHOD"));
	pos = appendData(pos, end, state.methodStr);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_NAME"));
	pos = appendData(pos, end, state.serverName);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_PORT"));
	pos = appendData(pos, end, state.serverPort);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_SOFTWARE"));
	pos = appendData(pos, end, serverSoftware);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_PROTOCOL"));
	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("HTTP/1.1"));

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REMOTE_ADDR"));
	if (state.remoteAddr != NULL) {
		pos = appendData(pos, end, state.remoteAddr);
		pos = appendData(pos, end, "", 1);
	} else {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("127.0.0.1"));
	}

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REMOTE_PORT"));
	if (state.remotePort != NULL) {
		pos = appendData(pos, end, state.remotePort);
		pos = appendData(pos, end, "", 1);
	} else {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("0"));
	}

	if (state.remoteUser != NULL) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REMOTE_USER"));
		pos = appendData(pos, end, state.remoteUser);
		pos = appendData(pos, end, "", 1);
	}

	if (state.contentLength != NULL) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("CONTENT_LENGTH"));
		pos = appendData(pos, end, state.contentLength);
		pos = appendData(pos, end, "", 1);
	}

	if (state.contentType != NULL) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("CONTENT_TYPE"));
		pos = appendData(pos, end, state.contentType);
		pos = appendData(pos, end, "", 1);
	}

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PASSENGER_CONNECT_PASSWORD"));
	pos = appendData(pos, end, req->session->getGroupSecret());
	pos = appendData(pos, end, "", 1);

	if (req->https) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("HTTPS"));
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("on"));
	}

	if (req->options.analytics) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PASSENGER_TXN_ID"));
		pos = appendData(pos, end, req->options.transaction->getTxnId());
		pos = appendData(pos, end, "", 1);
	}

	ServerKit::HeaderTable::Iterator it(req->headers);
	while (*it != NULL) {
		if ((it->header->hash == HTTP_CONTENT_LENGTH.hash()
			|| it->header->hash == HTTP_CONTENT_TYPE.hash()
			|| it->header->hash == HTTP_CONNECTION.hash())
		 && (psg_lstr_cmp(&it->header->key, P_STATIC_STRING("content-type"))
			|| psg_lstr_cmp(&it->header->key, P_STATIC_STRING("content-length"))
			|| psg_lstr_cmp(&it->header->key, P_STATIC_STRING("connection"))))
		{
			it.next();
			continue;
		}

		pos = appendData(pos, end, P_STATIC_STRING("HTTP_"));
		const LString::Part *part = it->header->key.start;
		while (part != NULL) {
			char *start = pos;
			pos = appendData(pos, end, part->data, part->size);
			httpHeaderToScgiUpperCase((unsigned char *) start, pos - start);
			part = part->next;
		}
		pos = appendData(pos, end, "", 1);

		part = it->header->val.start;
		while (part != NULL) {
			pos = appendData(pos, end, part->data, part->size);
			part = part->next;
		}
		pos = appendData(pos, end, "", 1);

		it.next();
	}

	Uint32Message::generate(buffer, pos - buffer - sizeof(boost::uint32_t));

	size = pos - buffer;
	return pos < end;
}

void
httpHeaderToScgiUpperCase(unsigned char *data, unsigned int size) {
	static const boost::uint8_t toUpperMap[256] = {
		'\0', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, '\t',
		'\n', 0x0b, 0x0c, '\r', 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
		0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
		0x1e, 0x1f,  ' ',  '!',  '"',  '#',  '$',  '%',  '&', '\'',
		 '(',  ')',  '*',  '+',  ',',  '_',  '.',  '/',  '0',  '1',
		 '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  ':',  ';',
		 '<',  '=',  '>',  '?',  '@',  'A',  'B',  'C',  'D',  'E',
		 'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',  'N',  'O',
		 'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',
		 'Z',  '[', '\\',  ']',  '^',  '_',  '`',  'A',  'B',  'C',
		 'D',  'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',
		 'N',  'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',
		 'X',  'Y',  'Z',  '{',  '|',  '}',  '~', 0x7f, 0x80, 0x81,
		0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
		0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
		0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
		0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
		0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd,
		0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
		0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1,
		0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
		0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5,
		0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
		0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
	};

	const unsigned char *buf = data;
	const size_t imax = size / 8;
	const size_t leftover = size % 8;
	size_t i;

	for (i = 0; i < imax; i++, data += 8) {
		data[0] = (unsigned char) toUpperMap[data[0]];
		data[1] = (unsigned char) toUpperMap[data[1]];
		data[2] = (unsigned char) toUpperMap[data[2]];
		data[3] = (unsigned char) toUpperMap[data[3]];
		data[4] = (unsigned char) toUpperMap[data[4]];
		data[5] = (unsigned char) toUpperMap[data[5]];
		data[6] = (unsigned char) toUpperMap[data[6]];
		data[7] = (unsigned char) toUpperMap[data[7]];
	}

	i = imax * 8;
	switch (leftover) {
	case 7: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 6: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 5: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 4: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 3: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 2: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 1: *data++ = (unsigned char) toUpperMap[buf[i]];
	case 0: break;
	}
}

struct HttpHeaderConstructionCache {
	StaticString methodStr;
	const LString *remoteAddr;
	bool cached;
};

void
sendHeaderToAppWithHttpProtocol(Client *client, Request *req) {
	ssize_t bytesWritten;
	HttpHeaderConstructionCache cache;

	cache.cached = false;

	if (!sendHeaderToAppWithHttpProtocolAndWritev(req, bytesWritten, cache)) {
		if (bytesWritten >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
			sendHeaderToAppWithSessionProtocolWithBuffering(req, bytesWritten, cache);
		} else {
			int e = errno;
			P_ASSERT_EQ(bytesWritten, -1);
			onAppInputError(NULL, e);
		}
	}
}

/**
 * Construct an array of buffers, which together contain the 'http' protocol header
 * data that should be sent to the application. This method does not copy any data:
 * it just constructs buffers that point to the data stored inside `req->pool`,
 * `req->headers`, etc.
 *
 * The buffers will be stored in the array pointed to by `buffer`. This array must
 * have space for at least `maxbuffers` items. The actual number of buffers constructed
 * is stored in `nbuffers`, and the total data size of the buffers is stored in `dataSize`.
 * Upon success, returns true. If the actual number of buffers necessary exceeds
 * `maxbuffers`, then false is returned.
 *
 * You can also set `buffers` to NULL, in which case this method will not construct any
 * buffers, but only count the number of buffers necessary, as well as the total data size.
 * In this case, this method always returns true.
 */
bool
constructHeaderBuffersForHttpProtocol(Request *req, struct iovec *buffers,
	unsigned int maxbuffers, unsigned int & restrict_ref nbuffers,
	unsigned int & restrict_ref dataSize, HttpHeaderConstructionCache &cache)
{
	#define INC_BUFFER_ITER(i) \
		do { \
			if (buffers != NULL && i == maxbuffers) { \
				return false; \
			} \
			i++; \
		} while (false)
	#define PUSH_STATIC_BUFFER(buf) \
		do { \
			if (buffers != NULL) { \
				buffers[i].iov_base = (void *) buf; \
				buffers[i].iov_len  = sizeof(buf) - 1; \
			} \
			INC_BUFFER_ITER(i); \
			dataSize += sizeof(buf) - 1; \
		} while (false)

	ServerKit::HeaderTable::Iterator it(req->headers);
	const LString::Part *part;
	unsigned int i = 0;

	nbuffers = 0;
	dataSize = 0;

	if (!cache.cached) {
		cache.methodStr  = http_method_str(req->method);
		cache.remoteAddr = req->secureHeaders.lookup(REMOTE_ADDR);
		cache.cached     = true;
	}

	if (buffers != NULL) {
		buffers[i].iov_base = (void *) cache.methodStr.data();
		buffers[i].iov_len  = cache.methodStr.size();
	}
	INC_BUFFER_ITER(i);
	dataSize += cache.methodStr.size();

	PUSH_STATIC_BUFFER(" ");

	if (buffers != NULL) {
		buffers[i].iov_base = (void *) req->path.start->data;
		buffers[i].iov_len  = req->path.size;
	}
	INC_BUFFER_ITER(i);
	dataSize += req->path.size;

	PUSH_STATIC_BUFFER(" HTTP/1.1\r\nConnection: close\r\n");

	while (*it != NULL) {
		if (it->header->hash == HTTP_CONNECTION.hash()
		 && psg_lstr_cmp(&it->header->key, P_STATIC_STRING("connection")))
		{
			it.next();
			continue;
		}

		part = it->header->key.start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}
		dataSize += it->header->key.size;

		PUSH_STATIC_BUFFER(": ");

		part = it->header->val.start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}
		dataSize += it->header->val.size;

		PUSH_STATIC_BUFFER("\r\n");

		it.next();
	}

	if (req->https) {
		PUSH_STATIC_BUFFER("X-Forwarded-Proto: https\r\n");
	}

	if (cache.remoteAddr != NULL && cache.remoteAddr->size > 0) {
		PUSH_STATIC_BUFFER("X-Forwarded-For: ");

		part = cache.remoteAddr->start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}
		dataSize += cache.remoteAddr->size;

		PUSH_STATIC_BUFFER("\r\n");
	}

	if (req->options.analytics) {
		PUSH_STATIC_BUFFER("Passenger-Txn-Id: ");

		if (buffers != NULL) {
			buffers[i].iov_base = (void *) req->options.transaction->getTxnId().data();
			buffers[i].iov_len  = req->options.transaction->getTxnId().size();
		}
		INC_BUFFER_ITER(i);
		dataSize += req->options.transaction->getTxnId().size();

		PUSH_STATIC_BUFFER("\r\n");
	}

	PUSH_STATIC_BUFFER("\r\n");

	nbuffers = i;
	return true;

	#undef INC_BUFFER_ITER
	#undef PUSH_STATIC_BUFFER
}

bool
sendHeaderToAppWithHttpProtocolAndWritev(Request *req, ssize_t &bytesWritten,
	HttpHeaderConstructionCache &cache)
{
	unsigned int maxbuffers = std::min<unsigned int>(
		4 + req->headers.size() * 4 + 4, IOV_MAX);
	struct iovec *buffers = (struct iovec *) psg_palloc(req->pool,
		sizeof(struct iovec) * maxbuffers);
	unsigned int nbuffers, dataSize;

	if (constructHeaderBuffersForHttpProtocol(req, buffers,
		maxbuffers, nbuffers, dataSize, cache))
	{
		ssize_t ret;
		do {
			ret = writev(req->session->fd(), buffers, nbuffers);
		} while (ret == -1 && errno == EINTR);
		bytesWritten = ret;
		return ret == (ssize_t) dataSize;
	} else {
		bytesWritten = 0;
		return false;
	}
}

void
sendHeaderToAppWithSessionProtocolWithBuffering(Request *req, unsigned int offset,
	HttpHeaderConstructionCache &cache)
{
	struct iovec *buffers;
	unsigned int nbuffers, dataSize;
	bool ok;

	ok = constructHeaderBuffersForHttpProtocol(req, NULL, 0, nbuffers, dataSize, cache);
	assert(ok);

	buffers = (struct iovec *) psg_palloc(req->pool,
		sizeof(struct iovec) * nbuffers);
	ok = constructHeaderBuffersForHttpProtocol(req, buffers, nbuffers,
		nbuffers, dataSize, cache);
	assert(ok);
	(void) ok; // Shut up compiler warning

	MemoryKit::mbuf_pool &mbuf_pool = getContext()->mbuf_pool;
	const unsigned int MBUF_MAX_SIZE = mbuf_pool.mbuf_block_chunk_size -
		mbuf_pool.mbuf_block_offset;
	if (dataSize <= MBUF_MAX_SIZE) {
		MemoryKit::mbuf buffer(MemoryKit::mbuf_get(&mbuf_pool));
		gatherBuffers(buffer.start, MBUF_MAX_SIZE, buffers, nbuffers);
		buffer = MemoryKit::mbuf(buffer, offset, dataSize - offset);
		req->appSink.feed(buffer);
	} else {
		char *buffer = (char *) psg_pnalloc(req->pool, dataSize);
		gatherBuffers(buffer, dataSize, buffers, nbuffers);
		req->appSink.feed(buffer + offset, dataSize - offset);
	}
}

void
sendBodyToApp(Client *client, Request *req) {
	TRACE_POINT();
	if (req->hasBody() || req->upgraded()) {
		// onRequestBody() will take care of forwarding
		// the request body to the app.
		SKC_TRACE(client, 2, "Sending body to application");
		req->state = Request::FORWARDING_BODY_TO_APP;
		startBodyChannel(client, req);
	} else {
		// Our task is done. ForwardResponse.cpp will take
		// care of ending the request, once all response
		// data is forwarded.
		SKC_TRACE(client, 2, "No body to send to application");
		req->state = Request::WAITING_FOR_APP_OUTPUT;
		maybeHalfCloseAppInput(client, req);
	}
}

Channel::Result
whenSendingRequest_onRequestBody(Client *client, Request *req,
	const MemoryKit::mbuf &buffer, int errcode)
{
	TRACE_POINT();

	if (buffer.size() > 0) {
		// Data
		SKC_TRACE(client, 3, "Forwarding " << buffer.size() <<
			" bytes of client request body: \"" <<
			cEscapeString(StaticString(buffer.start, buffer.size())) <<
			"\"");
		req->appSink.feed(buffer);
		if (!req->appSink.ended()) {
			if (req->appSink.passedThreshold()) {
				req->appSink.setBuffersFlushedCallback(resumeRequestBodyChannelWhenBuffersFlushed);
				stopBodyChannel(client, req);
			}
			return Channel::Result(buffer.size(), false);
		} else {
			return Channel::Result(buffer.size(), true);
		}
	} else if (errcode == 0 || errcode == ECONNRESET) {
		// EOF
		SKC_TRACE(client, 2, "End of request body encountered");
		// Our task is done. ForwardResponse.cpp will take
		// care of ending the request, once all response
		// data is forwarded.
		req->state = Request::WAITING_FOR_APP_OUTPUT;
		maybeHalfCloseAppInput(client, req);
		return Channel::Result(0, true);
	} else {
		const unsigned int BUFSIZE = 1024;
		char *message = (char *) psg_pnalloc(req->pool, BUFSIZE);
		int size = snprintf(message, BUFSIZE,
			"error reading request body: %s (errno=%d)",
			ServerKit::getErrorDesc(errcode), errcode);
		disconnectWithError(&client, StaticString(message, size));
		return Channel::Result(0, true);
	}
}

static void
resumeRequestBodyChannelWhenBuffersFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdSinkChannel *channel =
		reinterpret_cast<FileBufferedFdSinkChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(getServerFromClient(client));
	SKC_LOG_EVENT_FROM_STATIC(self, RequestHandler, client, "resumeRequestBodyChannelWhenBuffersFlushed");

	P_ASSERT_EQ(req->state, Request::FORWARDING_BODY_TO_APP);

	req->appSink.setBuffersFlushedCallback(NULL);
	self->startBodyChannel(client, req);
}

void
startBodyChannel(Client *client, Request *req) {
	if (req->requestBodyBuffering) {
		req->bodyBuffer.start();
	} else {
		req->bodyChannel.start();
	}
}

void
stopBodyChannel(Client *client, Request *req) {
	if (req->requestBodyBuffering) {
		req->bodyBuffer.stop();
	} else {
		req->bodyChannel.stop();
	}
}

void
maybeHalfCloseAppInput(Client *client, Request *req) {
	P_ASSERT_EQ(req->state, Request::WAITING_FOR_APP_OUTPUT);
	if (req->halfCloseAppConnection) {
		if (!req->appSink.ended()) {
			req->appSink.feed(MemoryKit::mbuf());
		}
		if (req->appSink.endAcked()) {
			halfCloseAppInput(client, req);
		} else {
			req->appSink.setDataFlushedCallback(halfCloseAppInputWhenDataFlushed);
		}
	}
}

void
halfCloseAppInput(Client *client, Request *req) {
	SKC_TRACE(client, 3, "Half-closing application socket with SHUT_WR");
	assert(req->halfCloseAppConnection);
	::shutdown(req->session->fd(), SHUT_WR);
}

static void
halfCloseAppInputWhenDataFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdSinkChannel *channel =
		reinterpret_cast<FileBufferedFdSinkChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(
		getServerFromClient(client));
	SKC_LOG_EVENT_FROM_STATIC(self, RequestHandler, client, "halfCloseAppInputWhenDataFlushed");

	P_ASSERT_EQ(req->state, Request::WAITING_FOR_APP_OUTPUT);

	req->appSink.setDataFlushedCallback(NULL);
	self->halfCloseAppInput(client, req);
}

void
whenOtherCases_onAppInputError(Client *client, Request *req, int errcode) {
	assert(req->state == Request::SENDING_HEADER_TO_APP
		|| req->state == Request::FORWARDING_BODY_TO_APP
		|| req->state == Request::WAITING_FOR_APP_OUTPUT);

	if (errcode == EPIPE) {
		// We consider an EPIPE non-fatal: we don't care whether the
		// app stopped reading, we just care about its output.
		SKC_DEBUG(client, "cannot write to application socket: "
			"the application closed the socket prematurely");
	} else if (req->responseBegun) {
		const unsigned int BUFSIZE = 1024;
		char *message = (char *) psg_pnalloc(req->pool, BUFSIZE);
		int size = snprintf(message, BUFSIZE,
			"cannot write to application socket: %s (errno=%d)",
			ServerKit::getErrorDesc(errcode), errcode);
		disconnectWithError(&client, StaticString(message, size));
	} else {
		SKC_WARN(client, "Cannot write to application socket: " <<
			ServerKit::getErrorDesc(errcode) << " (errcode=" << errcode << ")");
		endRequestAsBadGateway(&client, &req);
	}
}
