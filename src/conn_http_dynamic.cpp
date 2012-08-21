/// \file conn_http_dynamic.cpp
/// Contains the main code for the HTTP Dynamic Connector

#include <iostream>
#include <queue>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>
#include <ctime>
#include <mist/socket.h>
#include <mist/http_parser.h>
#include <mist/json.h>
#include <mist/dtsc.h>
#include <mist/flv_tag.h>
#include <mist/base64.h>
#include <mist/amf.h>
#include <mist/mp4.h>
#include <mist/config.h>

/// Holds everything unique to HTTP Dynamic Connector.
namespace Connector_HTTP{

  /// Returns a F4M-format manifest file
  std::string BuildManifest(std::string MovieId) {
    std::string Result="<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<manifest xmlns=\"http://ns.adobe.com/f4m/1.0\">\n"
      "<id>" + MovieId + "</id>\n"
      "<mimeType>video/mp4</mimeType>\n"
      "<streamType>live</streamType>\n"
      "<deliveryType>streaming</deliveryType>\n"
      "<bootstrapInfo profile=\"named\" id=\"bootstrap1\">" + Base64::encode(MP4::GenerateLiveBootstrap(1)) + "</bootstrapInfo>\n"
      "<media streamId=\"1\" bootstrapInfoId=\"bootstrap1\" url=\"" + MovieId + "/\">\n"
      "</media>\n"
      "</manifest>\n";
    return Result;
  }//BuildManifest

  /// Main function for Connector_HTTP_Dynamic
  int Connector_HTTP_Dynamic(Socket::Connection conn){
    std::string FlashBuf;
    FLV::Tag tmp;//temporary tag, for init data

    std::queue<std::string> Flash_FragBuffer;//Fragment buffer
    DTSC::Stream Strm;//Incoming stream buffer.
    HTTP::Parser HTTP_R, HTTP_S;//HTTP Receiver en HTTP Sender.

    bool ready4data = false;//Set to true when streaming is to begin.
    bool inited = false;
    Socket::Connection ss(-1);
    std::string streamname;
    FLV::Tag tag;///< Temporary tag buffer.
    std::string recBuffer = "";

    std::string Movie;
    std::string Quality;
    int Segment = -1;
    int ReqFragment = -1;
    int temp;
    int Flash_RequestPending = 0;
    unsigned int lastStats = 0;
    conn.setBlocking(false);//do not block on conn.spool() when no data is available

    while (conn.connected()){
      if (conn.spool()){
        if (HTTP_R.Read(conn.Received())){
          #if DEBUG >= 4
          std::cout << "Received request: " << HTTP_R.url << std::endl;
          #endif
          if (HTTP_R.url.find("f4m") == std::string::npos){
            streamname = HTTP_R.url.substr(1,HTTP_R.url.find("/",1)-1);
            Quality = HTTP_R.url.substr( HTTP_R.url.find("/",1)+1 );
            Quality = Quality.substr(0, Quality.find("Seg"));
            temp = HTTP_R.url.find("Seg") + 3;
            Segment = atoi( HTTP_R.url.substr(temp,HTTP_R.url.find("-",temp)-temp).c_str());
            temp = HTTP_R.url.find("Frag") + 4;
            ReqFragment = atoi( HTTP_R.url.substr(temp).c_str() );
            #if DEBUG >= 4
            printf( "Quality: %s, Seg %d Frag %d\n", Quality.c_str(), Segment, ReqFragment);
            #endif
            Flash_RequestPending++;
          }else{
            streamname = HTTP_R.url.substr(1,HTTP_R.url.find("/",1)-1);
            HTTP_S.Clean();
            HTTP_S.SetHeader("Content-Type","text/xml");
            HTTP_S.SetHeader("Cache-Control","no-cache");
            std::string manifest = BuildManifest(Movie);
            HTTP_S.SetBody(manifest);
            conn.Send(HTTP_S.BuildResponse("200", "OK"));
            #if DEBUG >= 3
            printf("Sent manifest\n");
            #endif
          }
          ready4data = true;
          HTTP_R.Clean(); //clean for any possible next requests
        }else{
          #if DEBUG >= 3
          fprintf(stderr, "Could not parse the following:\n%s\n", conn.Received().c_str());
          #endif
        }
      }
      if (ready4data){
        if (!inited){
          //we are ready, connect the socket!
          ss = Socket::getStream(streamname);
          if (!ss.connected()){
            #if DEBUG >= 1
            fprintf(stderr, "Could not connect to server!\n");
            #endif
            ss.close();
            HTTP_S.Clean();
            HTTP_S.SetBody("No such stream is available on the system. Please try again.\n");
            conn.Send(HTTP_S.BuildResponse("404", "Not found"));
            ready4data = false;
            continue;
          }
          #if DEBUG >= 3
          fprintf(stderr, "Everything connected, starting to send video data...\n");
          #endif
          inited = true;
        }
        if ((Flash_RequestPending > 0) && !Flash_FragBuffer.empty()){
          HTTP_S.Clean();
          HTTP_S.SetHeader("Content-Type","video/mp4");
          HTTP_S.SetBody(MP4::mdatFold(Flash_FragBuffer.front()));
          Flash_FragBuffer.pop();
          conn.Send(HTTP_S.BuildResponse("200", "OK"));
          Flash_RequestPending--;
          #if DEBUG >= 3
          fprintf(stderr, "Sending a video fragment. %i left in buffer, %i requested\n", (int)Flash_FragBuffer.size(), Flash_RequestPending);
          #endif
        }
        unsigned int now = time(0);
        if (now != lastStats){
          lastStats = now;
          ss.Send("S "+conn.getStats("HTTP_Dynamic"));
        }
        if (ss.spool() || ss.Received() != ""){
          if (Strm.parsePacket(ss.Received())){
            tag.DTSCLoader(Strm);
            if (Strm.getPacket(0).getContentP("keyframe")){
              if (FlashBuf != ""){
                Flash_FragBuffer.push(FlashBuf);
                while (Flash_FragBuffer.size() > 2){
                  Flash_FragBuffer.pop();
                }
                #if DEBUG >= 4
                fprintf(stderr, "Received a fragment. Now %i in buffer.\n", (int)Flash_FragBuffer.size());
                #endif
              }
              FlashBuf.clear();
              //fill buffer with init data, if needed.
              if (Strm.metadata.getContentP("audio") && Strm.metadata.getContentP("audio")->getContentP("init")){
                tmp.DTSCAudioInit(Strm);
                FlashBuf.append(tmp.data, tmp.len);
              }
              if (Strm.metadata.getContentP("video") && Strm.metadata.getContentP("video")->getContentP("init")){
                tmp.DTSCVideoInit(Strm);
                FlashBuf.append(tmp.data, tmp.len);
              }
            }
            FlashBuf.append(tag.data, tag.len);
          }
        }
        if (!ss.connected()){break;}
      }
    }
    conn.close();
    ss.close();
    #if DEBUG >= 1
    if (FLV::Parse_Error){fprintf(stderr, "FLV Parser Error: %s\n", FLV::Error_Str.c_str());}
    fprintf(stderr, "User %i disconnected.\n", conn.getSocket());
    if (inited){
      fprintf(stderr, "Status was: inited\n");
    }else{
      if (ready4data){
        fprintf(stderr, "Status was: ready4data\n");
      }else{
        fprintf(stderr, "Status was: connected\n");
      }
    }
    #endif
    return 0;
  }//Connector_HTTP_Dynamic main function

};//Connector_HTTP_Dynamic namespace

int main(int argc, char ** argv){
  Util::Config conf(argv[0], PACKAGE_VERSION);
  conf.addConnectorOptions(1935);
  conf.parseArgs(argc, argv);
  Socket::Server server_socket = Socket::Server("/tmp/mist/http_dynamic");
  if (!server_socket.connected()){return 1;}
  conf.activate();
  
  while (server_socket.connected() && conf.is_active){
    Socket::Connection S = server_socket.accept();
    if (S.connected()){//check if the new connection is valid
      pid_t myid = fork();
      if (myid == 0){//if new child, start MAINHANDLER
        return Connector_HTTP::Connector_HTTP_Dynamic(S);
      }else{//otherwise, do nothing or output debugging text
        #if DEBUG >= 3
        fprintf(stderr, "Spawned new process %i for socket %i\n", (int)myid, S.getSocket());
        #endif
      }
    }
  }//while connected
  server_socket.close();
  return 0;
}//main