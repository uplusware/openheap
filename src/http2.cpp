/*
	Copyright (c) openheap, uplusware
	uplusware@gmail.com
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include "util/huffman.h"
#include <time.h>
#include "http2.h"
#include "hpack.h"

#define PRE_MALLOC_SIZE 1024

CHttp2::CHttp2(ServiceObjMap* srvobj, int sockfd, const char* servername, unsigned short serverport,
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
    
    m_lsockfd = NULL;
    m_lssl = NULL;
    
    if(m_ssl)
	{
		int flags = fcntl(m_sockfd, F_GETFL, 0); 
		fcntl(m_sockfd, F_SETFL, flags | O_NONBLOCK);
		
		m_lssl = new linessl(m_sockfd, m_ssl);
	}
	else
	{
		int flags = fcntl(m_sockfd, F_GETFL, 0); 
		fcntl(m_sockfd, F_SETFL, flags | O_NONBLOCK); 

		m_lsockfd = new linesock(m_sockfd);
	}
    
    m_path = "";
    m_method = "";
    m_authority = "";
    m_scheme = "";
    
    init_header_table();
    
    int ret = HttpRecv(m_preface, HTTP2_PREFACE_LEN);
	m_preface[HTTP2_PREFACE_LEN] = '\0';
	
    if(ret != HTTP2_PREFACE_LEN)
    {
        throw(new string("HTTP2: Wrong client preface."));
        return;
    }
    
    printf("Client Preface: %s\n", m_preface);
    
    char * server_preface = (char*)malloc(sizeof(HTTP2_Frame) + sizeof(HTTP2_Setting));
    
	HTTP2_Frame* preface_frame = (HTTP2_Frame*)server_preface;
	preface_frame->length.len3b[0] = 0x00;
    preface_frame->length.len3b[1] = 0x00;
    preface_frame->length.len3b[2] = 0x06; //length is 6
	preface_frame->type = HTTP2_FRAME_TYPE_SETTINGS;
	preface_frame->flags = HTTP2_FRAME_FLAG_UNSET;
	preface_frame->r = HTTP2_FRAME_R_UNSET;
	preface_frame->indentifier = HTTP2_FRAME_INDENTIFIER_WHOLE;
	
    HTTP2_Setting* preface_setting = (HTTP2_Setting*)(server_preface + sizeof(HTTP2_Frame));
    preface_setting->identifier = htons(HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
    preface_setting->value = htonl(100);
	if(HttpSend(server_preface, sizeof(HTTP2_Frame) + sizeof(HTTP2_Setting)) != 0)
    {
        free(server_preface);
        throw(new string("HTTP2: Wrong server preface."));
        return;
    }
    free(server_preface);
	
    send_window_update(0, 1024*1024*16*100);
}

CHttp2::~CHttp2()
{          
    for(int x; x < m_HpackList.size(); x++)
    {
        delete m_HpackList[x];
    }
    
    for(int x; x < m_HttpList.size(); x++)
    {
        delete m_HttpList[x];
    }
    
    if(m_lssl)
        delete m_lssl;
    
    if(m_lsockfd)
        delete m_lsockfd;
}

void CHttp2::send_setting_ack(uint_32 stream_ind)
{
    HTTP2_Frame setting_ack;
    setting_ack.length.len3b[0] = 0x00;
    setting_ack.length.len3b[1] = 0x00;
    setting_ack.length.len3b[2] = 0x00; //length is 0
    setting_ack.type = HTTP2_FRAME_TYPE_SETTINGS;
    setting_ack.flags = HTTP2_FRAME_FLAG_SETTING_ACK;
    setting_ack.r = HTTP2_FRAME_R_UNSET;
    setting_ack.indentifier = htonl(stream_ind) >> 1;
    
    printf("  Send SETTING Ack for %d\n", ntohl(setting_ack.indentifier) >> 1);
    HttpSend((const char*)&setting_ack, sizeof(HTTP2_Frame));
}

void CHttp2::send_window_update(uint_32 stream_ind, uint_32 window_size)
{
    HTTP2_Frame window_update;
    window_update.length.len3b[0] = 0x00;
    window_update.length.len3b[1] = 0x00;
    window_update.length.len3b[2] = sizeof(HTTP2_Frame_Window_Update); //length is 8
    window_update.type = HTTP2_FRAME_TYPE_WINDOW_UPDATE;
    window_update.flags = HTTP2_FRAME_FLAG_UNSET;
    window_update.r = HTTP2_FRAME_R_UNSET;
    window_update.indentifier = htonl(stream_ind) >> 1;
    
    HTTP2_Frame_Window_Update window_update_frm;
    window_update_frm.r = 0;
    window_update_frm.win_size = htonl(window_size) >> 1;
    
    HttpSend((const char*)&window_update, sizeof(HTTP2_Frame));
    HttpSend((const char*)&window_update_frm, sizeof(HTTP2_Frame_Window_Update));
}

void CHttp2::ParseHeaders(uint_32 stream_ind, hpack* hdr)
{
    string str_line;
    //printf("decode header size: %d\n", hdr->m_decoded_headers.size());
    for(int x = 0; x < hdr->m_decoded_headers.size(); x++)
    {
        BOOL found_it = FALSE;
        map<int, pair<string, string> >::iterator it;
        if(hdr->m_decoded_headers[x].index > 0)
        {
            if(hdr->m_decoded_headers[x].index > m_header_static_table.size())
            {
                /*map<int, pair<string, string> >::iterator g;
                for(g = m_header_dynamic_table.begin(); g!= m_header_dynamic_table.end(); g++)
                {
                    printf("%d <%s>:[%s]\n", g->first, g->second.first.c_str(), g->second.second.c_str());
                }*/
                it = m_header_dynamic_table.find(hdr->m_decoded_headers[x].index);
                if(it != m_header_dynamic_table.end())
                    found_it = TRUE;
                
                //printf("dynamic: %u\n", hdr->m_decoded_headers[x].index);
            }
            else
            {
                it = m_header_static_table.find(hdr->m_decoded_headers[x].index);
                hdr->m_decoded_headers[x].name = it->second.first.c_str();
                if(hdr->m_decoded_headers[x].index_type == type_indexed)
                    hdr->m_decoded_headers[x].value = it->second.second.c_str();
                if(it != m_header_static_table.end())
                    found_it = TRUE;
                //printf("static: %u\n", hdr->m_decoded_headers[x].index);
            }
        }
        
        if(found_it)
        {
            /*printf("%u, %s %s [%s] [%s]\n", hdr->m_decoded_headers[x].index_type, 
            it->second.first.c_str(),  it->second.second.c_str(),
            hdr->m_decoded_headers[x].name.c_str(), hdr->m_decoded_headers[x].value.c_str());*/
            if(strcasecmp(it->second.first.c_str(), ":method") == 0)
            {
                if(hdr->m_decoded_headers[x].index_type == type_indexed)
                    m_method = it->second.second.c_str();
                else if(hdr->m_decoded_headers[x].index_type == type_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_without_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_never_indexed_indexed_name)
                    m_method = hdr->m_decoded_headers[x].value.c_str();
                if(m_method != "" && m_path != "")
                {
                    str_line = m_method;
                    str_line += " ";
                    str_line += m_path;
                    str_line += " HTTP/1.1\r\n";
                    //printf(str_line.c_str());
                    m_HttpList[stream_ind]->LineParse(str_line.c_str());
                }
            }
            else if(strcasecmp(it->second.first.c_str(), ":scheme") == 0)
            {
                
            }
            else if(strcasecmp(it->second.first.c_str(), ":path") == 0)
            {
                if(hdr->m_decoded_headers[x].index_type == type_indexed)
                    m_path = it->second.second.c_str();
                else if(hdr->m_decoded_headers[x].index_type == type_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_without_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_never_indexed_indexed_name)
                    m_path = hdr->m_decoded_headers[x].value.c_str();
                
                if(m_method != "" && m_path != "")
                {
                    str_line = m_method;
                    str_line += " ";
                    str_line += m_path;
                    str_line += " HTTP/1.1\r\n";
                    //printf(str_line.c_str());
                    m_HttpList[stream_ind]->LineParse(str_line.c_str());
                }
            }
            else if(strcasecmp(it->second.first.c_str(), ":authority") == 0)
            {
                if(hdr->m_decoded_headers[x].index_type == type_indexed)
                    m_authority = it->second.second.c_str();
                else if(hdr->m_decoded_headers[x].index_type == type_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_without_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_never_indexed_indexed_name)
                    m_authority = hdr->m_decoded_headers[x].value.c_str();
            }
            else
            {
                str_line = it->second.first.c_str();
                str_line += ": ";
                if(hdr->m_decoded_headers[x].index_type == type_indexed)
                    str_line += it->second.second.c_str();
                else if(hdr->m_decoded_headers[x].index_type == type_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_without_indexing_indexed_name
                    || hdr->m_decoded_headers[x].index_type == type_never_indexed_indexed_name)
                    str_line += hdr->m_decoded_headers[x].value.c_str();
                str_line += "\r\n";
                m_HttpList[stream_ind]->LineParse(str_line.c_str());
                //printf("@@@@: %s", str_line.c_str());
            }
        }
        else
        {
            if(strcasecmp(hdr->m_decoded_headers[x].name.c_str(), ":method") == 0)
            {
                m_method = hdr->m_decoded_headers[x].value.c_str();
                
                if(m_method != "" && m_path != "")
                {
                    str_line = m_method;
                    str_line += " ";
                    str_line += m_path;
                    str_line += " HTTP/1.1\r\n";
                    //printf("%d: %s\n", stream_ind, str_line.c_str());
                    m_HttpList[stream_ind]->LineParse(str_line.c_str());
                }
            }
            else if(strcasecmp(hdr->m_decoded_headers[x].name.c_str(), ":scheme") == 0)
            {
                
            }
            else if(strcasecmp(hdr->m_decoded_headers[x].name.c_str(), ":path") == 0)
            {
                m_path = hdr->m_decoded_headers[x].value.c_str();
                
                if(m_method != "" && m_path != "")
                {
                    str_line = m_method;
                    str_line += " ";
                    str_line += m_path;
                    str_line += " HTTP/1.1\r\n";
                    //printf(str_line.c_str());
                    m_HttpList[stream_ind]->LineParse(str_line.c_str());
                }
            }
            else if(strcasecmp(hdr->m_decoded_headers[x].name.c_str(), ":authority") == 0)
            {
                m_authority = hdr->m_decoded_headers[x].value.c_str();
            }
            else
            {
                if(strcmp(hdr->m_decoded_headers[x].name.c_str(), "") != 0
                    && strcmp(hdr->m_decoded_headers[x].value.c_str(), "") != 0)
                {
                    str_line = hdr->m_decoded_headers[x].name.c_str();
                    str_line += ": ";
                    str_line += hdr->m_decoded_headers[x].value.c_str();
                    str_line += "\r\n";
                    m_HttpList[stream_ind]->LineParse(str_line.c_str());
                    printf(str_line.c_str());
                }
            }
        }
        
        int stable_size = m_header_static_table.size();
        if(hdr->m_decoded_headers[x].index_type == type_indexing_indexed_name || hdr->m_decoded_headers[x].index_type == type_indexing_new_name)
        {
            if(strcmp(hdr->m_decoded_headers[x].name.c_str(), "") != 0)
            {
                //default table element size is 4096 (not octets here)
                int dtable_size = m_header_dynamic_table.size();
                //printf("m_header_dynamic_table size: %u\n", m_header_dynamic_table.size());
                for(int y = (dtable_size > 4095 ? 4095 : dtable_size); y >= 1; y--)
                {
                    //printf("y: %u\n", y);
                    usleep(100);
                    map<int, pair<string, string> >::iterator it_y;
                    it_y = m_header_dynamic_table.find(stable_size + y);
                    if(it_y != m_header_dynamic_table.end())
                    {
                        //printf(">>>>>>>>>>>>>>>>>>> dynamic table: %d\n", y + 1);
                        m_header_dynamic_table[stable_size + y + 1] = m_header_dynamic_table[stable_size + y];
                    }
                }
                m_header_dynamic_table[stable_size + 1] = make_pair(hdr->m_decoded_headers[x].name, hdr->m_decoded_headers[x].value);
            }
        }
    }
    if(m_HttpList[stream_ind]->GetMethod() != hmPost)
    {
        m_HttpList[stream_ind]->Response();
    }
}

int CHttp2::HttpSend(const char* buf, int len)
{
    //printf("%s\n", buf);
	if(m_ssl)
		return SSLWrite(m_sockfd, m_ssl, buf, len);
	else
		return _Send_( m_sockfd, buf, len);
		
}

int CHttp2::HttpRecv(char* buf, int len)
{
    //printf("%s\n", buf);
	if(m_ssl)
	{
		//printf("m_lssl->drecv(%p, %d)\n", buf, len);
		return m_lssl->drecv(buf, len);
	}
	else
		return m_lsockfd->drecv(buf, len);	
}

int CHttp2::ProtRecv()
{
    HTTP2_Frame* frame_hdr = (HTTP2_Frame*)malloc(sizeof(HTTP2_Frame));
	memset(frame_hdr, 0, sizeof(HTTP2_Frame));
	
    char* payload = NULL;
    const char*frame_names[]  = {"DATA", "HEADERS", "PRIORITY", "RST_STREAM", "SETTINGS", "PUSH_PROMISE", "PING", "GOAWAY", "WINDOW_UPDATE", "CONTINUATION"};
	int ret = HttpRecv((char*)frame_hdr, sizeof(HTTP2_Frame));
	if(ret == sizeof(HTTP2_Frame))
	{
		uint_32 payload_len = frame_hdr->length.len24;
        payload_len = ntohl(payload_len << 8);
        uint_32 stream_ind = frame_hdr->indentifier;
        stream_ind = ntohl(stream_ind << 1);
		printf("\r\n\r\n>>>> FRAME(%03d): length(%05d) type(%s)\n", stream_ind, payload_len, frame_names[frame_hdr->type]);
        
        if(stream_ind > 0)
        {
            map<uint_32, CHttp*>::iterator it = m_HttpList.find(stream_ind);
            if(it == m_HttpList.end())
            {
                m_HttpList[stream_ind] = new CHttp(m_srvobj,
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
                                                m_ssl, this, stream_ind);
            }
            else if(m_HttpList[stream_ind] == NULL)
            {
                m_HttpList[stream_ind] = new CHttp(m_srvobj,
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
                                                m_ssl, this, stream_ind);
            }
        }
        
		if(payload_len > 0)
        {
            payload = (char*)malloc(payload_len + 1);
            memset(payload, 0, payload_len + 1);
            ret = HttpRecv(payload, payload_len);
            /* printf("ret %d, payload_len %d\n", ret, payload_len); */
            if(ret != payload_len)
                goto END_SESSION;;
        }
        
        if(frame_hdr->type == HTTP2_FRAME_TYPE_GOAWAY)
        {
            printf("  Recieved HTTP2_FRAME_TYPE_GOAWAY\n");
            HTTP2_Frame_Goaway* frm_goaway = (HTTP2_Frame_Goaway*)payload;
            uint_32 last_stream_id = frm_goaway->last_stream_id;
            last_stream_id = htonl(last_stream_id << 31);
            uint_32 error_code = frm_goaway->error_code;
            error_code = htonl(error_code);
            
            /* printf("GOAWAY: %u %u", last_stream_id, error_code); */
            goto END_SESSION;
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_PRIORITY)
        {
            HTTP2_Frame_Priority * prority = (HTTP2_Frame_Priority *)payload;
            uint_32 dep = ntohl(prority->dependency << 1);
            
            printf("  Recv PRIORITY for %d as Weight %d\n", ntohl(prority->dependency << 1), prority->weight);
            
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_HEADERS)
        {
            uint_32 padding_len = 0;
            uint_32 dep = 0;
            uint_8 weight;
            int offset = 0;
            if( (frame_hdr->flags & HTTP2_FRAME_FLAG_PADDED) == HTTP2_FRAME_FLAG_PADDED)
            {
                printf("  There's some >> PAD <<!\n");
                HTTP2_Frame_Header_Pad * header1 = (HTTP2_Frame_Header_Pad* )payload;
                padding_len = header1->pad_length;
                offset += sizeof(HTTP2_Frame_Header_Pad);
                
                /* printf("padding_len: %d\n", padding_len); */
            }
            
            if((frame_hdr->flags & HTTP2_FRAME_FLAG_PRIORITY) == HTTP2_FRAME_FLAG_PRIORITY)
            {
                printf("  There's >> PRIORITY << !\n");
                HTTP2_Frame_Header_Weight * header2 = (HTTP2_Frame_Header_Weight*)(payload + offset);
                dep = ntohl(header2->dependency << 1);
                weight = header2->weight;
                offset += sizeof(HTTP2_Frame_Header_Weight);
            }
            
            int fragment_len = payload_len - offset - padding_len;
            
            //printf("padding %u, offset %d, fragment_len %u\n", padding_len, offset, fragment_len);
            HTTP2_Frame_Header_Fragment * header3 = (HTTP2_Frame_Header_Fragment*)(payload + offset);

			/*for(int x = 0; x < fragment_len; x++)
            {
                printf("%02x ", header3->block_fragment[x]);
                if(x%16 == 15)
                    printf("\n");
            }
            printf("\n");*/
            
            if(stream_ind > 0)
            {
                map<uint_32, hpack*>::iterator it2 = m_HpackList.find(stream_ind);
                if(it2 == m_HpackList.end())
                {
                    m_HpackList[stream_ind] = new hpack();
                }
                else if(m_HpackList[stream_ind] == NULL)
                {
                    m_HpackList[stream_ind] = new hpack();
                }
                
                m_HpackList[stream_ind]->parse((HTTP2_Header_Field*)header3->block_fragment, fragment_len);
                
                if((frame_hdr->flags & HTTP2_FRAME_FLAG_END_HEADERS) == HTTP2_FRAME_FLAG_END_HEADERS)
                {
                    printf("  Recieved HTTP2_FRAME_FLAG_END_HEADERS\n");
                    ParseHeaders(stream_ind, m_HpackList[stream_ind]);
                    if(m_HpackList[stream_ind] != NULL)
                        delete m_HpackList[stream_ind];
                    m_HpackList[stream_ind] = NULL;
                }
                if((frame_hdr->flags & HTTP2_FRAME_FLAG_END_STREAM) == HTTP2_FRAME_FLAG_END_STREAM)
                {
                    delete m_HttpList[stream_ind];
                    m_HttpList[stream_ind] = NULL;
                }
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_CONTINUATION)
        {
            HTTP2_Frame_Continuation * continuation = (HTTP2_Frame_Continuation*)(payload);
            
            int fragment_len = payload_len;
            
            if(stream_ind > 0)
            {
                map<uint_32, hpack*>::iterator it3 = m_HpackList.find(stream_ind);
                if(it3 == m_HpackList.end())
                {
                    m_HpackList[stream_ind] = new hpack();
                }
                else if(m_HpackList[stream_ind] == NULL)
                {
                    m_HpackList[stream_ind] = new hpack();
                }
                
                m_HpackList[stream_ind]->parse((HTTP2_Header_Field*)continuation->block_fragment, fragment_len);
                
                if((frame_hdr->flags & HTTP2_FRAME_FLAG_END_HEADERS) == HTTP2_FRAME_FLAG_END_HEADERS)
                {
                    printf("  Recieved HTTP2_FRAME_FLAG_END_HEADERS\n");
                    ParseHeaders(stream_ind, m_HpackList[stream_ind]);
                    if(m_HpackList[stream_ind] != NULL)
                        delete m_HpackList[stream_ind];
                    m_HpackList[stream_ind] = NULL;
                }
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_PUSH_PROMISE)
        {
            uint_32 padding_len = 0;
            uint_32 dep = 0;
            int offset = 0;
            
            uint_32 promised_stream_ind = 0;
            if( (frame_hdr->flags & HTTP2_FRAME_FLAG_PADDED) == HTTP2_FRAME_FLAG_PADDED)
            {
                HTTP2_Frame_Push_Promise * header1 = (HTTP2_Frame_Push_Promise* )payload;
                padding_len = header1->pad_length;
                offset += sizeof(HTTP2_Frame_Push_Promise);
                
                promised_stream_ind = header1->promised_stream_ind;
            }
            else
            {
                HTTP2_Frame_Push_Promise_Without_Pad * header2 = (HTTP2_Frame_Push_Promise_Without_Pad* )payload;
                offset += sizeof(HTTP2_Frame_Push_Promise_Without_Pad);
                promised_stream_ind = header2->promised_stream_ind;
            }
            
            promised_stream_ind = ntohl(promised_stream_ind << 1);
            //printf("offset, %d\n", offset);
            
            HTTP2_Frame_Push_Promise_Fragment * push_promise = (HTTP2_Frame_Push_Promise_Fragment*)(payload + offset);
            
            int fragment_len = payload_len - offset - padding_len;
            
            if(promised_stream_ind > 0)
            {
                map<uint_32, hpack*>::iterator it = m_HpackList.find(promised_stream_ind);
                if(it == m_HpackList.end())
                {
                    m_HpackList[promised_stream_ind] = new hpack();
                }
                else if(m_HpackList[promised_stream_ind] == NULL)
                {
                    m_HpackList[promised_stream_ind] = new hpack();
                }
                
                m_HpackList[promised_stream_ind]->parse((HTTP2_Header_Field*)push_promise->block_fragment, fragment_len);
                
                if((frame_hdr->flags & HTTP2_FRAME_FLAG_END_HEADERS) == HTTP2_FRAME_FLAG_END_HEADERS)
                {
                    printf("  Recieved HTTP2_FRAME_FLAG_END_HEADERS\n");
                    ParseHeaders(promised_stream_ind, m_HpackList[promised_stream_ind]);
                    if(m_HpackList[promised_stream_ind] != NULL)
                        delete m_HpackList[promised_stream_ind];
                    m_HpackList[promised_stream_ind] = NULL;
                }
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_SETTINGS)
        {
            HTTP2_Setting* client_setting = (HTTP2_Setting*)payload;
            /*for(int x = 0; x < payload_len/sizeof(HTTP2_Setting); x++)
            {
                printf("    client_setting(%d): %d %d\n", x, ntohs(client_setting[x].identifier), ntohl(client_setting[x].value));
            }*/
            if((frame_hdr->flags & HTTP2_FRAME_FLAG_SETTING_ACK) != HTTP2_FRAME_FLAG_SETTING_ACK) //non setting ack
            {
                HTTP2_Frame setting_ack;
                setting_ack.length.len3b[0] = 0x00;
                setting_ack.length.len3b[1] = 0x00;
                setting_ack.length.len3b[2] = 0x00; //length is 0
                setting_ack.type = HTTP2_FRAME_TYPE_SETTINGS;
                setting_ack.flags = HTTP2_FRAME_FLAG_SETTING_ACK;
                setting_ack.r = HTTP2_FRAME_R_UNSET;
                setting_ack.indentifier = frame_hdr->indentifier;
                
                printf("  Send Ack for %d\n", ntohl(setting_ack.indentifier << 1));
                
                HttpSend((const char*)&setting_ack, sizeof(HTTP2_Frame));
            }
            else
            {
                printf("  This is a SETTING Ack\n");
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_DATA)
        {
            int offset = 0;
            uint_32 padding_len = 0;
            if( (frame_hdr->flags & HTTP2_FRAME_FLAG_PADDED) == HTTP2_FRAME_FLAG_PADDED)
            {
                HTTP2_Frame_Data1 * data1 = (HTTP2_Frame_Data1* )payload;
                padding_len = data1->pad_length;
                offset += sizeof(HTTP2_Frame_Data1);
                
                m_HttpList[stream_ind]->PushPostData(data1->data_padding, payload_len - padding_len);
            }
            else
            {
                HTTP2_Frame_Data2 * data2 = (HTTP2_Frame_Data2* )payload;
                offset += sizeof(HTTP2_Frame_Data2);
                m_HttpList[stream_ind]->PushPostData(data2->data, payload_len);
            }
            m_HttpList[stream_ind]->Response();
            
            if( (frame_hdr->flags & HTTP2_FRAME_FLAG_END_STREAM) == HTTP2_FRAME_FLAG_END_STREAM)
            {
                delete m_HttpList[stream_ind];
                m_HttpList[stream_ind] = NULL;
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_WINDOW_UPDATE)
        {
            HTTP2_Frame_Window_Update* win_update = (HTTP2_Frame_Window_Update*)payload;
            uint_32 win_size = ntohl(win_update->win_size << 1);
            printf("  Window Size: %u\n", win_size);
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_RST_STREAM)
        {
            if(stream_ind > 0)
            {
                map<uint_32, hpack*>::iterator it_hpack = m_HpackList.find(stream_ind);
                if(it_hpack != m_HpackList.end())
                {
                    if(m_HpackList[stream_ind] != NULL)
                        delete m_HpackList[stream_ind];
                    m_HpackList[stream_ind] = NULL;
                }
                
                map<uint_32, CHttp*>::iterator it_http = m_HttpList.find(stream_ind);
                if(it_http != m_HttpList.end())
                {
                    if(m_HttpList[stream_ind] != NULL)
                        delete m_HttpList[stream_ind];
                    m_HttpList[stream_ind] = NULL;
                }
            }
        }
        else if(frame_hdr->type == HTTP2_FRAME_TYPE_PING)
        {
            if((frame_hdr->flags & HTTP2_FRAME_FLAG_PING_ACK) != HTTP2_FRAME_FLAG_PING_ACK) //non setting ack
            {
                HTTP2_Frame* ping_ack = (HTTP2_Frame*)malloc(sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame_Ping));
                ping_ack->length.len3b[0] = 0x00;
                ping_ack->length.len3b[1] = 0x00;
                ping_ack->length.len3b[2] = sizeof(HTTP2_Frame_Ping); //length is 8
                ping_ack->type = HTTP2_FRAME_TYPE_PING;
                ping_ack->flags = HTTP2_FRAME_FLAG_PING_ACK;
                ping_ack->r = HTTP2_FRAME_R_UNSET;
                ping_ack->indentifier = frame_hdr->indentifier;
                
                HTTP2_Frame_Ping* ping_ack_frm = (HTTP2_Frame_Ping*)((char*)ping_ack + sizeof(HTTP2_Frame));
                HTTP2_Frame_Ping* ping_frm = (HTTP2_Frame_Ping*)payload;
                memcpy(ping_ack_frm->data, ping_frm->data, sizeof(HTTP2_Frame_Ping));
                printf("  Send PING Ack\n");
                HttpSend((const char*)ping_ack, sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame_Ping));
                free(ping_ack);
            }
            else
            {
                printf("  This is a PING Ack\n");
            }
        }
        if(payload)
            free(payload);
		return ret;
	}
END_SESSION:
    if(payload)
        free(payload);
    return -1;
}

Http_Connection CHttp2::Processing()
{
    Http_Connection httpConn = httpKeepAlive;
    while(1)
    {
        int result = ProtRecv();
        if(result <= 0)
        {
            /* printf("result: %d\n", result); */
            httpConn = httpClose;
            break;
        }
    }
    return httpConn;
}

int CHttp2::ParseHttp1Header(uint_32 stream_ind, const char* buf, int len)
{
    char* response_buf = (char*)malloc(sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame) + PRE_MALLOC_SIZE);
    HTTP2_Frame * http2_frm_hdr = (HTTP2_Frame *)response_buf;
    uint_32 response_len = sizeof(HTTP2_Frame);
    uint_32 response_buff_size = sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame) + PRE_MALLOC_SIZE;
    
    string line_text = buf;
    while(1)
    {
        string strtext;
        std::size_t new_line = line_text.find('\n');
        if( new_line == std::string::npos)
        {
            break;
        }
        else
        {
            strtext = line_text.substr(0, new_line + 1);
            line_text = line_text.substr(new_line + 1);
        }

        strtrim(strtext);
        
        // Skip empty line
        if(strtext == "")
            continue;
        
        HTTP2_Header_Field field;
        
        HTTP2_Header_String* hdr_string = NULL;
        uint_32 hdr_string_len = 0;
        
        index_type_e index_type = type_never_indexed_new_name;
        field.tag.never_indexed.code = 0x01;
        field.tag.never_indexed.index = 0x0;
            
        if(strncasecmp(strtext.c_str(), "HTTP/1.1", 8) == 0)
        {
            string strtextlow;
            lowercase(strtext.c_str(), strtextlow);
            char status_code[16];
            memset(status_code, 0, 16);
            //printf("%s\n", strtextlow.c_str());
            sscanf(strtextlow.c_str(), "http/1.1%*[^0-9]%[0-9]%*[^0-9]", status_code);
            printf("  :status %s\n", status_code);
                    
            for(map<int, pair<string, string> >::iterator it = m_header_static_table.begin(); it != m_header_static_table.end(); ++it)
            {
                if(strcasecmp(it->second.first.c_str(), ":status") == 0)
                {
                    index_type = type_indexing_indexed_name;
                    field.tag.indexing.code = 0x01;
                    field.tag.indexing.index = it->first;
                    if(strcasecmp(it->second.second.c_str(), status_code) == 0)
                    {
                        index_type = type_indexed;
                        field.tag.indexed.code = 0x01;
                        field.tag.indexed.index = it->first;
                        //printf("!!!!!!!!!!! status found: %s\n", status_code);
                        break;
                    }
                    
                    //printf("%d %s\n", it->first, it->second.first.c_str());
                }
            }
            if(index_type == type_indexing_indexed_name)
            {                
                const char* string_buf = status_code;
                int string_len = strlen(status_code);
                
                hdr_string = (HTTP2_Header_String*)malloc(sizeof(HTTP2_Header_String) + MAX_BYTES_OF_LENGTH + MAX_HUFFMAN_BUFF_LEN(string_len));
                
                //printf("!!!!!!!!!! ENCODE1: %s %d\n", string_buf, string_len);
                int out_len = 0;
                unsigned char* out_buff = (unsigned char*)malloc(MAX_HUFFMAN_BUFF_LEN(string_len));
                memset(out_buff, 0, MAX_HUFFMAN_BUFF_LEN(string_len));
                
                NODE* h_node;
                hf_init(&h_node);
                hf_string_encode(string_buf, string_len, 0, out_buff, &out_len);                
                hf_finish(h_node);

                char* string_ptr = NULL;
                int integer_len = encode_http2_header_string(hdr_string, out_len, &string_ptr);
                
                hdr_string->h = 1;
                
                memcpy(string_ptr, out_buff, out_len);
                
                free(out_buff);
                
                hdr_string_len = integer_len + out_len;
            }
        }
        else
        {
            string strName, strValue;
            strcut(strtext.c_str(), NULL, ":", strName);
            strcut(strtext.c_str(), ":", NULL, strValue);
            strtrim(strName);
            strtrim(strValue);
            
            string strNameLow;
            lowercase(strName.c_str(), strNameLow);

            for(map<int, pair<string, string> >::iterator it = m_header_static_table.begin(); it != m_header_static_table.end(); ++it)
            {
                if(strcasecmp(it->second.first.c_str(), strNameLow.c_str()) == 0)
                {
                    index_type = type_indexing_indexed_name;
                    field.tag.indexing.code = 0x01;
                    field.tag.indexing.index = it->first;
                    if(strcasecmp(it->second.second.c_str(), strValue.c_str()) == 0)
                    {
                        index_type = type_indexed;
                        field.tag.indexed.code = 0x01;
                        field.tag.indexed.index = it->first;
                        break;
                    }
                    //printf("%d %s\n", it->first, it->second.first.c_str());
                }
            }
            
            if(index_type == type_indexing_indexed_name)
            {
                const char*  string_buf = strValue.c_str();
                int string_len = strValue.length();

                hdr_string = (HTTP2_Header_String*)malloc(sizeof(HTTP2_Header_String) + MAX_BYTES_OF_LENGTH + MAX_HUFFMAN_BUFF_LEN(string_len));
                
                //printf("!!!!!!!!!! ENCODE2: %s %d\n", string_buf, string_len);
                int out_len = 0;
                unsigned char* out_buff = (unsigned char*)malloc(MAX_HUFFMAN_BUFF_LEN(string_len));
                memset(out_buff, 0, MAX_HUFFMAN_BUFF_LEN(string_len));
                
                NODE* h_node;
                hf_init(&h_node);
                hf_string_encode(string_buf, string_len, 0, out_buff, &out_len);                
                hf_finish(h_node);

                char* string_ptr = NULL;
                int integer_len = encode_http2_header_string(hdr_string, out_len, &string_ptr);
                
                hdr_string->h = 1;
                
                memcpy(string_ptr, out_buff, out_len);
                
                free(out_buff);
                
                //printf("integer_len: %d out_len: %d %p %p\n", integer_len, out_len, hdr_string, string_ptr);
                hdr_string_len = integer_len + out_len;
            }
            else if(index_type == type_never_indexed_new_name)
            {
                const char*  string_buf = strNameLow.c_str();
                int string_len = strNameLow.length();
                 
                const char*  string_buf2 = strValue.c_str();
                int string_len2 = strValue.length();
                
                hdr_string = (HTTP2_Header_String*)malloc(sizeof(HTTP2_Header_String) + MAX_BYTES_OF_LENGTH + MAX_HUFFMAN_BUFF_LEN(string_len) + MAX_HUFFMAN_BUFF_LEN(string_len2));
                
                //printf("!!!!!!!!!! ENCODE3: %s %d\n", string_buf, string_len);
                int out_len = 0;
                unsigned char* out_buff = (unsigned char*)malloc(MAX_HUFFMAN_BUFF_LEN(string_len));
                memset(out_buff, 0, MAX_HUFFMAN_BUFF_LEN(string_len));
                
                NODE* h_node;
                hf_init(&h_node);
                hf_string_encode(string_buf, string_len, 0, out_buff, &out_len);                
                hf_finish(h_node);

                char* string_ptr = NULL;
                int integer_len = encode_http2_header_string(hdr_string, out_len, &string_ptr);
                
                hdr_string->h = 1;
                
                memcpy(string_ptr, out_buff, out_len);
                
                free(out_buff);
                
                hdr_string_len = integer_len + out_len;

                
                HTTP2_Header_String* hdr_string2 = (HTTP2_Header_String*)((char*)hdr_string + integer_len + out_len);
                
                //printf("!!!!!!!!!! ENCODE3: %s %d\n", string_buf2, string_len2);
                int out_len2 = 0;
                unsigned char* out_buff2 = (unsigned char*)malloc(MAX_HUFFMAN_BUFF_LEN(string_len2));
                memset(out_buff2, 0, MAX_HUFFMAN_BUFF_LEN(string_len2));
                
                NODE* h_node2;
                hf_init(&h_node2);
                hf_string_encode(string_buf2, string_len2, 0, out_buff2, &out_len2);                
                hf_finish(h_node2);

                char* string_ptr2 = NULL;
                int integer_len2 = encode_http2_header_string(hdr_string2, out_len2, &string_ptr2);
                
                hdr_string2->h = 1;
                
                memcpy(string_ptr2, out_buff2, out_len2);
                
                free(out_buff2);
                
                hdr_string_len += integer_len2 + out_len2;
            }
        }
        
        if(response_buff_size < response_len + sizeof(HTTP2_Header_Field) + hdr_string_len)
        {
            char* response_buf_swap = (char*)malloc(response_len + sizeof(HTTP2_Header_Field) + hdr_string_len + PRE_MALLOC_SIZE);
            response_buff_size = response_len + sizeof(HTTP2_Header_Field) + hdr_string_len + PRE_MALLOC_SIZE;
            memcpy(response_buf_swap, response_buf, response_len);
            free(response_buf);
            response_buf = response_buf_swap;
        }
        memcpy(response_buf + response_len, &field, sizeof(HTTP2_Header_Field));
        response_len += sizeof(HTTP2_Header_Field);
        //printf("hdr_string_len: %u\n", hdr_string_len);
        if(hdr_string && hdr_string_len > 0)
        {
            memcpy(response_buf + response_len, hdr_string, hdr_string_len);
            response_len += hdr_string_len;
        }
    }
    uint_32 frame_len = response_len - sizeof(HTTP2_Frame);
    //printf("frame length: %u, stream_ind:　%d\n", frame_len, stream_ind);
    
    http2_frm_hdr->length.len24 = htonl(frame_len) >> 8;
    //printf("len: %02x, %02x, %02x\n", http2_frm_hdr->length.len3b[0], http2_frm_hdr->length.len3b[1], http2_frm_hdr->length.len3b[2]);
    http2_frm_hdr->type = HTTP2_FRAME_TYPE_HEADERS;
    http2_frm_hdr->flags = HTTP2_FRAME_FLAG_END_HEADERS;
    http2_frm_hdr->r = HTTP2_FRAME_R_UNSET;
    
    http2_frm_hdr->indentifier = htonl(stream_ind) >> 1;
    
    //printf("#1: %u\n", response_len);
    if(response_buff_size < response_len + sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame_Data2) + 2)
    {
        char* response_buf_swap = (char*)malloc(response_len + sizeof(HTTP2_Frame_Data2) + 2 + PRE_MALLOC_SIZE);
        response_buff_size = response_len + sizeof(HTTP2_Frame_Data2) + 2 + PRE_MALLOC_SIZE;
        memcpy(response_buf_swap, response_buf, response_len);
        free(response_buf);
        response_buf = response_buf_swap;
    }
    
    if(m_HttpList[stream_ind]->GetMethod() == hmHead)
    {
        HTTP2_Frame * http2_frm_data = (HTTP2_Frame *)(response_buf + response_len);
        response_len += sizeof(HTTP2_Frame);
        
        http2_frm_data->length.len24 = 0;//htonl(2) >> 8;
        //printf("len: %02x, %02x, %02x\n", http2_frm_data->length.len3b[0], http2_frm_data->length.len3b[1], http2_frm_data->length.len3b[2]);
        http2_frm_data->type = HTTP2_FRAME_TYPE_DATA;
        http2_frm_data->flags = HTTP2_FRAME_FLAG_UNSET;
        http2_frm_data->r = HTTP2_FRAME_R_UNSET;
        http2_frm_data->indentifier = htonl(stream_ind) >> 1;
        
        //printf("stream_ind:　%d\n", stream_ind);
    }
    
    return /* m_HttpList[stream_ind]->*/HttpSend(response_buf, response_len);
}

int CHttp2::ParseHttp1Content(uint_32 stream_ind, const char* buf, uint_32 len)
{
    int response_len;
    char* response_buf = (char*)malloc(sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame_Data2) + len);
    response_len = sizeof(HTTP2_Frame) + sizeof(HTTP2_Frame_Data2) + len;
    HTTP2_Frame * http2_frm_data = (HTTP2_Frame *)response_buf;
    http2_frm_data->length.len24 = htonl(len) >> 8;
    //printf("@@@@@@@@@HTTP2 Content, len: %02x, %02x, %02x\n", http2_frm_data->length.len3b[0], http2_frm_data->length.len3b[1], http2_frm_data->length.len3b[2]);
    http2_frm_data->type = HTTP2_FRAME_TYPE_DATA;
    http2_frm_data->flags = HTTP2_FRAME_FLAG_UNSET;
    http2_frm_data->r = HTTP2_FRAME_R_UNSET;
    
    http2_frm_data->indentifier = htonl(stream_ind) >> 1;
    
    //printf("stream_ind:　%d\n", stream_ind);
    
    HTTP2_Frame_Data2* data = (HTTP2_Frame_Data2*)(response_buf + sizeof(HTTP2_Frame));
    memcpy(data->data, buf, len);
    
    return /*m_HttpList[stream_ind]->*/HttpSend(response_buf, response_len);
}

int CHttp2::SendEmptyData(uint_32 stream_ind)
{
    int response_len;
    char* response_buf = (char*)malloc(sizeof(HTTP2_Frame));
    response_len = sizeof(HTTP2_Frame);
    HTTP2_Frame * http2_frm_data = (HTTP2_Frame *)response_buf;
    http2_frm_data->length.len24 = 0;
    //printf("len: %02x, %02x, %02x\n", http2_frm_data->length.len3b[0], http2_frm_data->length.len3b[1], http2_frm_data->length.len3b[2]);
    http2_frm_data->type = HTTP2_FRAME_TYPE_DATA;
    http2_frm_data->flags = HTTP2_FRAME_FLAG_END_STREAM;
    http2_frm_data->r = HTTP2_FRAME_R_UNSET;
    
    http2_frm_data->indentifier = htonl(stream_ind) >> 1;
    
    //printf("SendEmptyData: stream_ind:　%d\n", stream_ind);
    
    return /*m_HttpList[stream_ind]->*/HttpSend(response_buf, response_len);
}

void CHttp2::init_header_table()
{
	m_header_static_table[1] = make_pair(string(":authority"), string(""));
    m_header_static_table[2] = make_pair(string(":method"), string("GET"));
	m_header_static_table[3] = make_pair(string(":method"), string("POST"));
	m_header_static_table[4] = make_pair(string(":path"), string("/")); 
	m_header_static_table[5] = make_pair(string(":path"), string("/index.html"));
	m_header_static_table[6] = make_pair(string(":scheme"), string("http"));
	m_header_static_table[7] = make_pair(string(":scheme"), string("https")); 
	m_header_static_table[8] = make_pair(string(":status"), string("200"));
	m_header_static_table[9] = make_pair(string(":status"), string("204"));
	m_header_static_table[10] = make_pair(string(":status"), string("206"));
	m_header_static_table[11] = make_pair(string(":status"), string("304"));
	m_header_static_table[12] = make_pair(string(":status"), string("400"));
	m_header_static_table[13] = make_pair(string(":status"), string("404"));
	m_header_static_table[14] = make_pair(string(":status"), string("500"));
	m_header_static_table[15] = make_pair(string("accept-charset"), string(""));
	m_header_static_table[16] = make_pair(string("accept-encoding"), string("gzip, deflate"));
	m_header_static_table[17] = make_pair(string("accept-language"), string(""));
	m_header_static_table[18] = make_pair(string("accept-ranges"), string(""));
	m_header_static_table[19] = make_pair(string("accept"), string(""));
	m_header_static_table[20] = make_pair(string("access-control-allow-origin"), string(""));
	m_header_static_table[21] = make_pair(string("age"), string(""));
	m_header_static_table[22] = make_pair(string("allow"), string("")); 
	m_header_static_table[23] = make_pair(string("authorization"), string(""));
	m_header_static_table[24] = make_pair(string("cache-control"), string(""));
	m_header_static_table[25] = make_pair(string("content-disposition"), string(""));
	m_header_static_table[26] = make_pair(string("content-encoding"), string(""));
	m_header_static_table[27] = make_pair(string("content-language"), string(""));
	m_header_static_table[28] = make_pair(string("content-length"), string("")); 
	m_header_static_table[29] = make_pair(string("content-location"), string(""));
	m_header_static_table[30] = make_pair(string("content-range"), string(""));
	m_header_static_table[31] = make_pair(string("content-type"), string(""));
	m_header_static_table[32] = make_pair(string("cookie"), string(""));
	m_header_static_table[33] = make_pair(string("date"), string(""));
	m_header_static_table[34] = make_pair(string("etag"), string(""));
	m_header_static_table[35] = make_pair(string("expect"), string(""));
	m_header_static_table[36] = make_pair(string("expires"), string(""));
	m_header_static_table[37] = make_pair(string("from"), string(""));
	m_header_static_table[38] = make_pair(string("host"), string(""));
	m_header_static_table[39] = make_pair(string("if-match"), string("")); 
	m_header_static_table[40] = make_pair(string("if-modified-since"), string("")); 
	m_header_static_table[41] = make_pair(string("if-none-match"), string(""));
	m_header_static_table[42] = make_pair(string("if-range"), string("")); 
	m_header_static_table[43] = make_pair(string("if-unmodified-since"), string(""));
	m_header_static_table[44] = make_pair(string("last-modified"), string(""));
	m_header_static_table[45] = make_pair(string("link"), string(""));
	m_header_static_table[46] = make_pair(string("location"), string("")); 
	m_header_static_table[47] = make_pair(string("max-forwards"), string(""));
	m_header_static_table[48] = make_pair(string("proxy-authenticate"), string(""));
	m_header_static_table[49] = make_pair(string("proxy-authorization"), string(""));
	m_header_static_table[50] = make_pair(string("range"), string("")); 
	m_header_static_table[51] = make_pair(string("referer"), string(""));
	m_header_static_table[52] = make_pair(string("refresh"), string(""));
	m_header_static_table[53] = make_pair(string("retry-after"), string("")); 
	m_header_static_table[54] = make_pair(string("server"), string(""));
	m_header_static_table[55] = make_pair(string("set-cookie"), string(""));
	m_header_static_table[56] = make_pair(string("strict-transport-security"), string(""));
	m_header_static_table[57] = make_pair(string("transfer-encoding"), string("")); 
	m_header_static_table[58] = make_pair(string("user-agent"), string(""));
	m_header_static_table[59] = make_pair(string("vary"), string(""));
	m_header_static_table[60] = make_pair(string("via"), string(""));
	m_header_static_table[61] = make_pair(string("www-authenticate"), string(""));
}