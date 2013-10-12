/*
 * @f util/ccn-lite-cryptoserver.c
 * @b cryptoserver for functions for the CCN-lite
 *
 * Copyright (C) 2013, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2013-07-22 created
 */
#define CCNL_USE_MGMT_SIGNATUES

#include "../ccnl.h"
#include "../ccnx.h"
#include "ccnl-common.c"
#include "ccnl-crypto.c"

#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <sys/un.h>



char *ux_path, *private_key, *ctrl_public_key;

int ux_sendto(int sock, char *topath, unsigned char *data, int len)
{
    struct sockaddr_un name;
    int rc;

    /* Construct name of socket to send to. */
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, topath);

    /* Send message. */
    rc = sendto(sock, data, len, 0, (struct sockaddr*) &name,
		sizeof(struct sockaddr_un));
    if (rc < 0) {
      fprintf(stderr, "named pipe \'%s\'\n", topath);
      perror("sending datagram message");
    }
    return rc;
}

int
ux_open(char *frompath)
{
  int sock, bufsize;
    struct sockaddr_un name;

    /* Create socket for sending */
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
	perror("opening datagram socket");
	exit(1);
    }
    unlink(frompath);
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, frompath);
    if (bind(sock, (struct sockaddr *) &name,
	     sizeof(struct sockaddr_un))) {
	perror("binding name to datagram socket");
	exit(1);
    }
//    printf("socket -->%s\n", NAME);

    bufsize = 4 * CCNL_MAX_PACKET_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    return sock;
}

int 
get_tag_content(unsigned char **buf, int *len, char *content){
    int num = 0;
    while((**buf) !=  0)
    {
        content[num] = **buf;
        ++(*buf); --(*len);
        ++num;
    }
    ++(*buf); --(*len);
    return num;
}

int
handle_verify(char **buf, int *buflen, int sock){
    int num, typ, verified;
    int contentlen = 0;
    int siglen = 0;
    char *txid_s = 0, *sig = 0, *content = 0;
    int len = 0, len2 = 0, len3 = 0;
    char *component_buf, *contentobj_buf, *msg;
    char h[1024];
    
    //open content
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    while (dehead(buf, buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr2(txid_s, CCN_DTAG_SEQNO);
        if(!siglen)siglen = *buflen;
	extractStr2(sig, CCN_DTAG_SIGNATURE);
        siglen = siglen - (*buflen + 5);
        contentlen = *buflen;
        extractStr2(content, CCN_DTAG_CONTENTDIGEST);

	if (consume(typ, num, buf, buflen, 0, 0) < 0) goto Bail;
    }
    contentlen = contentlen - (*buflen + 4);
    
    printf("Contentlen: %d\n", contentlen);
    printf("Siglen: %d\n", siglen);
    
    
    printf("TXID:     %s\n", txid_s);
    printf("HASH:     %s\n", sig);
    printf("CONTENT:  %s\n", content);
    
    verified = verify(ctrl_public_key, content, contentlen, sig, siglen);
    printf("Verified: %d\n", verified);
    
    //return message object
    msg = ccnl_malloc(sizeof(char)*1000);
    component_buf = ccnl_malloc(sizeof(char)*666);
    contentobj_buf = ccnl_malloc(sizeof(char)*333);
    
    len = mkHeader(msg, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // ContentObj
    len += mkHeader(msg+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "crypto");
    
    // prepare FACEINSTANCE
    
    
    len3 += mkStrBlob(component_buf+len3, CCN_DTAG_SEQNO, CCN_TT_DTAG, txid_s);
    memset(h,0,sizeof(h));
    sprintf(h,"%d", verified);
    len3 += mkStrBlob(component_buf+len3, CCNL_DTAG_VERIFIED, CCN_TT_DTAG, h);
    
    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj_buf, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkStrBlob(contentobj_buf+len2, CCN_DTAG_TYPE, CCN_TT_DTAG, "verify");
    len2 += mkBlob(contentobj_buf+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) component_buf, len3);
    contentobj_buf[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj_buf, len2);

    msg[len++] = 0; // end-of-name
    msg[len++] = 0; // end-o
    
    memset(h,0,sizeof(h));
    sprintf(h,"%s-2", ux_path);
    ux_sendto(sock, h, msg, len);
    printf("answered to: %s len: %d\n", ux_path, len);
    Bail:
    ccnl_free(contentobj_buf);
    ccnl_free(component_buf);
    ccnl_free(msg);
    return verified;
}


int
handle_sign(char **buf, int *buflen, int sock){
    int num, typ, verified;
    int contentlen = 0;
    int siglen = 0;
    char *txid_s = 0, *sig = 0, *content = 0;
    int len = 0, len2 = 0, len3 = 0;
    char *component_buf, *contentobj_buf, *msg;
    char h[1024];
    
    //open content
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    while (dehead(buf, buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr2(txid_s, CCN_DTAG_SEQNO);
        contentlen = *buflen;
        extractStr2(content, CCN_DTAG_CONTENTDIGEST);

	if (consume(typ, num, buf, buflen, 0, 0) < 0) goto Bail;
    }
    contentlen = contentlen - (*buflen + 4);
    
    printf("Contentlen: %d\n", contentlen);
    
    
    printf("TXID:     %s\n", txid_s);
    printf("CONTENT:  %s\n", content);
    
    sig = ccnl_malloc(sizeof(char)*4096);
    verified = sign(ctrl_public_key, content, contentlen, sig, &siglen);
    printf("Verified: %d\n", verified);
    realloc(sig, siglen);
    //return message object
    
    msg = ccnl_malloc(sizeof(char)*siglen + 1000);
    component_buf = ccnl_malloc(sizeof(char)*siglen + 666);
    contentobj_buf = ccnl_malloc(sizeof(char)*siglen + 333);
    
    len = mkHeader(msg, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // ContentObj
    len += mkHeader(msg+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "crypto");
    
    // prepare FACEINSTANCE
    
    
    len3 += mkStrBlob(component_buf+len3, CCN_DTAG_SEQNO, CCN_TT_DTAG, txid_s);
    len3 += mkBlob(component_buf+len3, CCN_DTAG_SIGNATURE, CCN_TT_DTAG,  // signature
		   (char*) sig, siglen);
    
    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj_buf, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkStrBlob(contentobj_buf+len2, CCN_DTAG_TYPE, CCN_TT_DTAG, "sign");
    len2 += mkBlob(contentobj_buf+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) component_buf, len3);
    contentobj_buf[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj_buf, len2);

    msg[len++] = 0; // end-of-name
    msg[len++] = 0; // end-o
    
    memset(h,0,sizeof(h));
    sprintf(h,"%s-2", ux_path);
    ux_sendto(sock, h, msg, len);
    printf("answered to: %s len: %d\n", ux_path, len);
    Bail:
    ccnl_free(contentobj_buf);
    ccnl_free(component_buf);
    ccnl_free(msg);
    return verified;
}

int parse_crypto_packet(char *buf, int buflen, int sock){
    int num, typ;
    char component[100];
    char type[100];
    char content[64000];
    
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_INTEREST) goto Bail; 
    
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_NAME) goto Bail; 
    
    //check if component ist ccnx
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail; 
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    memset(component, 0, sizeof(component));
    get_tag_content(&buf, &buflen, component);
    if(strcmp(component, "ccnx")) goto Bail; 
    
    //check if component is crypto
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail; 
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    memset(component, 0, sizeof(component));
    get_tag_content(&buf, &buflen, component);
    if(strcmp(component, "crypto")) goto Bail;
   
    //open content object
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail; 
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail; 
    
    //get msg-type, what to do
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_TYPE) goto Bail; 
    if(dehead(&buf, &buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    memset(type, 0, sizeof(type));
    get_tag_content(&buf, &buflen, type);
    
    if(!strcmp(type, "verify"))
        handle_verify(&buf, &buflen, sock);
    
    if(!strcmp(type, "sign"))
        handle_sign(&buf, &buflen, sock);
    return 1;
    Bail:
    printf("foo\n");
    return 0;
    
}

int crypto_main_loop(int sock)
{
    //receive packet async and call parse/answer...
    int len;
    char buf[64000];
    struct sockaddr_un src_addr;
    socklen_t addrlen = sizeof(struct sockaddr_un);
    
    
    len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*) &src_addr,
            &addrlen);
    
    parse_crypto_packet(buf, len, sock);
    
    return 1;
}

int main(int argc, char **argv)
{
    struct sockaddr_un ux;
    if(argc < 3) goto Bail;
    ux_path = argv[1];
    ctrl_public_key = argv[2];
    if(argc >= 3)
        private_key = argv[3];
    
    int sock = ux_open(ux_path);
    while(crypto_main_loop(sock));
    
    Bail:
    printf("Usage: %s crypto_ux_socket_path"
        " public_key [private_key]\n", argv[0]);
}
