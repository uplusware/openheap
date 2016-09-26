#include "http2stream.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////
//http2_stream
http2_stream::http2_stream(uint_32 stream_ind, CHttp2* phttp2, ServiceObjMap* srvobj, int sockfd,
        const char* servername, unsigned short serverport,
	    const char* clientip, X509* client_cert, memory_cache* ch,
		const char* work_path, vector<stExtension>* ext_list, const char* php_mode, 
        const char* fpm_socktype, const char* fpm_sockfile, 
        const char* fpm_addr, unsigned short fpm_port, const char* phpcgi_path,
        const char* fastcgi_name, const char* fastcgi_pgm, 
        const char* fastcgi_socktype, const char* fastcgi_sockfile,
        const char* fastcgi_addr, unsigned short fastcgi_port,
        const char* private_path, unsigned int global_uid, AUTH_SCHEME wwwauth_scheme,
		SSL* ssl)
{
    m_path = "";
    m_method = "";
    m_authority = "";
    m_scheme = "";
    
    m_stream_ind = stream_ind;
    m_srvobj = srvobj;
    m_sockfd = sockfd;
    m_servername = servername;
    m_serverport = serverport;
    m_clientip = clientip;
    m_client_cert = client_cert;
    m_ch = ch;
    m_work_path = work_path;
    m_ext_list = ext_list;
    m_php_mode = php_mode;
    m_fpm_socktype = fpm_socktype;
    m_fpm_sockfile = fpm_sockfile;
    m_fpm_addr = fpm_addr;
    m_fpm_port = fpm_port;
    m_phpcgi_path = phpcgi_path;
    m_fastcgi_name = fastcgi_name;
    m_fastcgi_pgm = fastcgi_pgm;
    m_fastcgi_socktype = fastcgi_socktype;
    m_fastcgi_sockfile = fastcgi_sockfile;
    m_fastcgi_addr = fastcgi_addr;
    m_fastcgi_port = fastcgi_port;
    m_private_path = private_path;
    m_global_uid = global_uid;
    m_wwwauth_scheme = wwwauth_scheme;
    m_ssl = ssl;
    
    m_http2 = phttp2;
    
    m_http1 = new CHttp(m_srvobj,
                            m_sockfd,
                            m_servername.c_str(),
                            m_serverport,
                            m_clientip.c_str(),
                            m_client_cert,
                            m_ch,
                            m_work_path.c_str(),
                            m_ext_list,
                            m_php_mode.c_str(),
                            m_fpm_socktype.c_str(),
                            m_fpm_sockfile.c_str(),
                            m_fpm_addr.c_str(),
                            m_fpm_port,
                            m_phpcgi_path.c_str(),
                            m_fastcgi_name.c_str(),
                            m_fastcgi_pgm.c_str(),
                            m_fastcgi_socktype.c_str(),
                            m_fastcgi_sockfile.c_str(),
                            m_fastcgi_addr.c_str(),
                            m_fastcgi_port,
                            m_private_path.c_str(),
                            m_global_uid,
                            m_wwwauth_scheme,
                            m_ssl, m_http2, m_stream_ind);
    m_hpack = NULL;
    
    m_push_promise_trigger_header = "";
    
    m_dependency_stream = 0;
    m_stream_state = stream_idle;
}

http2_stream::~http2_stream()
{
    m_stream_state = stream_closed;
    
    if(m_http1)
        delete m_http1;
    if(m_hpack)
        delete m_hpack;
    m_http1 = NULL;
    m_hpack = NULL;
}

void http2_stream::BuildPushPromiseResponse(http2_stream* host_stream, const char* path)
{
    m_push_promise_trigger_header = "GET ";
    m_push_promise_trigger_header += path;
    m_push_promise_trigger_header += " HTTP/1.1\r\n";
    
    if(strcmp(host_stream->GetHttp1()->GetRequestField("Accept"), "") != 0)
    {
        m_push_promise_trigger_header += "Accept: ";
        m_push_promise_trigger_header += host_stream->GetHttp1()->GetRequestField("Accept");
        m_push_promise_trigger_header += "\r\n";
    }
    if(strcmp(host_stream->GetHttp1()->GetRequestField("Accept-Encoding"), "") != 0)
    {
        m_push_promise_trigger_header += "Accept-Encoding: ";
        m_push_promise_trigger_header += host_stream->GetHttp1()->GetRequestField("Accept-Encoding");
        m_push_promise_trigger_header += "\r\n";
    }
    if(strcmp(host_stream->GetHttp1()->GetRequestField("Accept-Language"), "") != 0)
    {
        m_push_promise_trigger_header += "Accept-Language: ";
        m_push_promise_trigger_header += host_stream->GetHttp1()->GetRequestField("Accept-Language");
        m_push_promise_trigger_header += "\r\n";
    }
    if(strcmp(host_stream->GetHttp1()->GetRequestField("User-Agent"), "") != 0)
    {
        m_push_promise_trigger_header += "User-Agent: ";
        m_push_promise_trigger_header += host_stream->GetHttp1()->GetRequestField("User-Agent");
        m_push_promise_trigger_header += "\r\n";
    }
    m_push_promise_trigger_header += "\r\n";
}

void http2_stream::SendPushPromiseResponse()
{
    if(m_push_promise_trigger_header != "")
        http1_parse(m_push_promise_trigger_header.c_str());
    m_push_promise_trigger_header = "";
}

int http2_stream::hpack_parse(HTTP2_Header_Field* field, int len)
{
    if(!m_hpack)
        m_hpack = new hpack();
    return m_hpack->parse(field, len);
}

Http_Connection http2_stream::http1_parse(const char* text)
{
    return m_http1->LineParse(text);
}

CHttp* http2_stream::GetHttp1()
{
    return m_http1;
}

hpack* http2_stream::GetHpack()
{
    return m_hpack;
}

Http_Method http2_stream::GetMethod()
{
    return m_http1->GetMethod();
}

void http2_stream::PushPostData(const char* buf, int len)
{
    m_http1->PushPostData(buf, len);
}

void http2_stream::Response()
{
    m_http1->Response();
}

void http2_stream::ClearHpack()
{
    if(m_hpack)
        delete m_hpack;
    m_hpack = NULL;
}

void http2_stream::SetPriorityWeight(uint_32 weight)
{
    m_priority_weight = weight;
}

void http2_stream::SetStreamState(stream_state_e state)
{
    m_stream_state = state;
}

stream_state_e http2_stream::GetStreamState()
{
    return m_stream_state;
}

void http2_stream::SetDependencyStream(uint_32 dependency_stream)
{
    m_dependency_stream = dependency_stream;
}

uint_32 http2_stream::GetDependencyStream()
{
    return m_dependency_stream;
}