/*======================================================== 
** University of Illinois/NCSA 
** Open Source License 
**
** Copyright (C) 2011,The Board of Trustees of the University of 
** Illinois. All rights reserved. 
**
** Developed by: 
**
**    Research Group of Professor Sam King in the Department of Computer 
**    Science The University of Illinois at Urbana-Champaign 
**    http://www.cs.uiuc.edu/homes/kingst/Research.html 
**
** Copyright (C) Sam King and Hui Xue
**
** Permission is hereby granted, free of charge, to any person obtaining a 
** copy of this software and associated documentation files (the 
** Software), to deal with the Software without restriction, including 
** without limitation the rights to use, copy, modify, merge, publish, 
** distribute, sublicense, and/or sell copies of the Software, and to 
** permit persons to whom the Software is furnished to do so, subject to 
** the following conditions: 
**
** Redistributions of source code must retain the above copyright notice, 
** this list of conditions and the following disclaimers. 
**
** Redistributions in binary form must reproduce the above copyright 
** notice, this list of conditions and the following disclaimers in the 
** documentation and/or other materials provided with the distribution. 
** Neither the names of Sam King or the University of Illinois, 
** nor the names of its contributors may be used to endorse or promote 
** products derived from this Software without specific prior written 
** permission. 
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
** IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR 
** ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
** SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE. 
**========================================================== 
*/

#include <iostream>
#include <queue>

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "MyServerSocket.h"
#include "HTTPRequest.h"
#include "Cache.h"
#include "dbg.h"
#include "time.h"

using namespace std;

int serverPorts[] = {8808, 8809, 8810};
#define NUM_SERVERS (sizeof(serverPorts) / sizeof(serverPorts[0]))

static string CONNECT_REPLY = "HTTP/1.1 200 Connection Established\r\n\r\n";

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long numThreads = 0;

struct client_struct {
        MySocket *sock;
        int serverPort;
        queue<MySocket *> *killQueue;
};

struct server_struct {
        int serverPort;
};

pthread_t server_threads[NUM_SERVERS];
static int gVOTING = 0;

void run_client(MySocket *sock, int serverPort)
{
        HTTPRequest *request = new HTTPRequest(sock, serverPort);
    
        if(!request->readRequest()) {
                cout << "did not read request" << endl;
        } else {    
                bool error = false;
                bool isSSL = false;

                string host = request->getHost();
                string url = request->getUrl();

                MySocket *replySock = NULL;
        
                if(request->isConnect()) {
                        //deal with MITM in later patches
                        assert(false);                        
                        if(!sock->write_bytes(CONNECT_REPLY)) {
                                error = true;
                        } else {
                                delete request;
                                replySock = cache()->getReplySocket(host, true);
                                // need proxy <--> remotesite socket
                                // for information needed to fake a
                                // certificate
                                //
                                // sock->enableSSLServer(replySock);
                                isSSL = true;
                                request = new HTTPRequest(sock, serverPort);
                                if(!request->readRequest()) {
                                        error = true;
                                }
                        }            
                } else {
                        replySock = cache()->getReplySocket(host, false);
                }        

                if(!error) {
                        string req = request->getRequest();
                        if(gVOTING == 0) {
                                cache()->getHTTPResponseNoVote(host, req, url, serverPort,
                                                               sock, isSSL, replySock);
                        } else {
                                //if(isSSL == true)
                                //cache()->getHTTPResponseVote(host, req, url, serverPort,
                                //sock, isSSL, replySock);
                                assert(false);
                                /*
                                cache()->getHTTPResponseVote(host, req, url, serverPort,
                                                             sock, isSSL, replySock);
                                */
                        }
            
                }
        }    

        sock->close();
        delete request;
}

void *client_thread(void *arg)
{
        struct client_struct *cs = (struct client_struct *) arg;
        MySocket *sock = cs->sock;
        int serverPort = cs->serverPort;
        queue<MySocket *> *killQueue = cs->killQueue;

        delete cs;
    
        pthread_mutex_lock(&mutex);
        numThreads++;
        pthread_mutex_unlock(&mutex);

        run_client(sock, serverPort);

        pthread_mutex_lock(&mutex);
        numThreads--;

        // This is a hack because linux is having trouble freeing
        // memory in a different thread, so instead we will let the
        // server thread free this memory
        killQueue->push(sock);
        pthread_mutex_unlock(&mutex);    

        return NULL;
}

void start_client(MySocket *sock, int serverPort, queue<MySocket *> *killQueue)
{
        struct client_struct *cs = new struct client_struct;
        cs->sock = sock;
        cs->serverPort = serverPort;
        cs->killQueue = killQueue;

        pthread_t tid;
        int ret = pthread_create(&tid, NULL, client_thread, cs);
        assert(ret == 0);
        ret = pthread_detach(tid);
        assert(ret == 0);
}

void *server_thread(void *arg)
{
        struct server_struct *ss = (struct server_struct *)arg;
        int port = ss->serverPort;
        delete ss;
    
        MyServerSocket *server = new MyServerSocket(port);
        assert(server != NULL);
        MySocket *client;
        queue<MySocket *> killQueue;
        while(true) {
                try {
                        client = server->accept();
                } catch(MySocketException e) {
                        cerr << e.toString() << endl;
                        exit(1);
                }
                pthread_mutex_lock(&mutex);
                while(killQueue.size() > 0) {
                        delete killQueue.front();
                        killQueue.pop();
                }
                pthread_mutex_unlock(&mutex);
                start_client(client, port, &killQueue);
        }    
        return NULL;
}


pthread_t start_server(int port)
{
        cerr << "starting server on port " << port << endl;
        server_struct *ss = new struct server_struct;
        ss->serverPort = port;
        pthread_t tid;
        int ret = pthread_create(&tid, NULL, server_thread, ss);
        assert(ret == 0);
        return tid;
}

static void get_opts(int argc, char *argv[])
{
        int c;
        while((c = getopt(argc, argv, "v")) != EOF) {
                switch(c) {
                case 'v':
                        gVOTING = 1;
                        break;
                default:
                        cerr << "Wrong Argument." << endl;
                        exit(1);
                        break;
                }
        }
}
int main(int argc, char *argv[])
{
        // if started with "-v" option, voting will be
        // enabled. Otherwise, just a plain proxy
        get_opts(argc, argv);  
        // get socket write errors from write call
        signal(SIGPIPE, SIG_IGN);

        // initialize ssl library
        SSL_load_error_strings();
        SSL_library_init();

        cout << "number of servers: " << NUM_SERVERS << endl;

        // when generating serial number for X509, need random number
        srand(time(NULL));
        Cache::setNumBrowsers(NUM_SERVERS);
    
        pthread_t tid;
        int ret;
        for(unsigned int idx = 0; idx < NUM_SERVERS; idx++) {
                tid = start_server(serverPorts[idx]);
                server_threads[idx] = tid;
        }

        for(unsigned int idx = 0; idx < NUM_SERVERS; idx++) {
                ret = pthread_join(server_threads[idx], NULL);
                assert(ret == 0);
        }
        return 0;
}
