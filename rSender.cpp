#include <stdio.h>
#include <string.h>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstring>
#include <time.h>
#include <map>
#include <fstream>

#include "PacketHeader.h"
#include "crc32.h"

#define DATABUFFERSIZE 1456
#define PACKETBUFFERSIZE 1472
#define HEADERSIZE 16
#define TOTALHEADERSIZE 44
#define TIMEOUT 500000

using namespace std; 

struct args {
  char* receiverIP;
  char* receiverPort;
  char* windowSize;
  char* inputFile;
  char* log;
};

struct socketInfo {
  int sockfd;
  struct sockaddr_in server_addr;
  socklen_t server_len;
};

struct packet : public PacketHeader {
  char data[DATABUFFERSIZE];
};

struct packetTracker {
  // <seqNum, packet>
  map<int, packet> unACKedPackets;
  map<int, packet> ACKedPackets;
  uint32_t highestACKSeqNum = 0;
};

auto retrieveArgs(char* argv[]) {
  args newArgs;
  newArgs.receiverIP = argv[1];
  newArgs.receiverPort = argv[2];
  newArgs.windowSize = argv[3];
  newArgs.inputFile = argv[4];
  newArgs.log = argv[5];
  return newArgs;
}

PacketHeader createSTARTPacket(){
  PacketHeader startHeader;
  startHeader.type = 0;
  srand(time(0));
  startHeader.seqNum = rand(); // used for START and END packet
  startHeader.length = 0; // 0 for ACKS, START, END
  startHeader.checksum = 0; // 0 for ACKS, START, END
  return startHeader;
}

PacketHeader createENDPacket(unsigned int seqNum){
  PacketHeader endHeader;
  endHeader.type = 1;
  endHeader.seqNum = seqNum; // Same as START seqNum
  endHeader.length = 0; // 0 for ACKS, START, END
  endHeader.checksum = 0; // 0 for ACKS, START, END
  return endHeader;
}

socketInfo setupSocket(char* portNum, char* host) {
  socketInfo newSocket;
  newSocket.sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (newSocket.sockfd < 0) {
    perror("ERROR creating socket");
    exit(1);
  }
  memset((char *) &newSocket.server_addr, 0, sizeof(newSocket.server_addr));
  newSocket.server_addr.sin_family = AF_INET;
  newSocket.server_addr.sin_port = htons(atoi(portNum));  // specify port to connect to 
  newSocket.server_len = sizeof(newSocket.server_addr);
  struct hostent* sp = gethostbyname(host);
  memcpy(&newSocket.server_addr.sin_addr, sp->h_addr, sp->h_length);
  return newSocket;
}

void setSocketTimeout(int sockfd, int timeout) {
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = timeout;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)) < 0) {
    perror("Error setting timeout");
    exit(1);
  }
}

packetTracker readFileInToTracker(char* inputFile) {
  packetTracker tracker;
  ifstream file(inputFile, ios::binary);
  if (!file.is_open()) {
    exit(1);
  }
  int seqNum = 0;
  while (!file.eof()) {
    packet newPacket;
    memset(newPacket.data,'\0', DATABUFFERSIZE);
    file.read(newPacket.data, DATABUFFERSIZE);
    streamsize bytesRead = file.gcount();
    newPacket.type = 2;
    newPacket.seqNum = seqNum;
    newPacket.length = bytesRead;
    newPacket.checksum = crc32(newPacket.data, bytesRead);
    tracker.unACKedPackets[seqNum] = newPacket;
    seqNum++;
  }
  file.close();
  return tracker;
}

void writeToLogFile(char* logFilePath, string type, string seqNum, string length, string checksum) {
  string logMessage = type + " " + seqNum + " " + length + " " + checksum + "\n";
  string fullFilePath = string(logFilePath);
  ofstream log(fullFilePath, ios_base::app);
  if (!log.is_open()) {
    exit(1);
  }
  // stream to output file
  log << logMessage;
  log.close();
}

bool sendSTART(socketInfo &socket, PacketHeader &startHeader, char* logFile) {
  if (sendto(socket.sockfd, (char*)&startHeader, sizeof(startHeader), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
  {
    printf("Error sending start\n");
    exit(1);
  }
  writeToLogFile(logFile, to_string(startHeader.type), to_string(startHeader.seqNum), to_string(startHeader.length), to_string(startHeader.checksum));
  return true;
}

bool getSTARTACK(socketInfo &socket, PacketHeader ACKPacket, char* logFile) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len)) {
    if (ACKPacket.type == 3) {
    writeToLogFile(logFile, to_string(ACKPacket.type), to_string(ACKPacket.seqNum), to_string(ACKPacket.length), to_string(ACKPacket.checksum));
    return true;
    }
  }
  return false;
}

bool sendEND(socketInfo &socket, PacketHeader &endHeader, char* logFile) {
  if (sendto(socket.sockfd, (char*)&endHeader, sizeof(endHeader), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
  {
    printf("Error sending end\n");
    exit(1);
  }
  // write to log file
  writeToLogFile(logFile, to_string(endHeader.type), to_string(endHeader.seqNum), to_string(endHeader.length), to_string(endHeader.checksum));
  return true;
}

bool getENDACK(socketInfo &socket, PacketHeader ACKPacket, char* logFile) {

  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len)) {
    if (ACKPacket.type == 3) {
      return true;
    }
  }
  // write to log file
  writeToLogFile(logFile, to_string(ACKPacket.type), to_string(ACKPacket.seqNum), to_string(ACKPacket.length), to_string(ACKPacket.checksum));
  return false;
}

PacketHeader receiveDataACK(socketInfo &socket, PacketHeader ACKPacket, char* logFile) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len) == -1)
  {
    return ACKPacket;
  } else {
    writeToLogFile(logFile, to_string(ACKPacket.type), to_string(ACKPacket.seqNum), to_string(ACKPacket.length), to_string(ACKPacket.checksum));
  }
  return ACKPacket;
}

bool rUDPSend(socketInfo &socket, char* windowSize, packetTracker &tracker, char* logFile) {

  bool sendLoop = true;
  int windowBegin = 0;
  int windowEnd = atoi(windowSize);
  int lastHighestSeqNum = 0;

  while(sendLoop) {
    // send all packets in window
    for (int i = windowBegin; i < windowEnd; i++) {
      // Guard for window size larger than number of packets to send
      if (tracker.unACKedPackets.find(i) == tracker.unACKedPackets.end()) {
        sendLoop = false;
        break;
      }

      // send packet
      if (sendto(socket.sockfd, (char*)&tracker.unACKedPackets[i], tracker.unACKedPackets[i].length + HEADERSIZE, 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
        {
          printf("Error sending data\n");
          exit(1);
        }
      // write to log
      writeToLogFile(logFile, to_string(tracker.unACKedPackets[i].type), to_string(tracker.unACKedPackets[i].seqNum), to_string(tracker.unACKedPackets[i].length), to_string(tracker.unACKedPackets[i].checksum));
    }

    lastHighestSeqNum = tracker.highestACKSeqNum;

    // collect all ACKs
    for (int i = 0; i < atoi(windowSize); i++) {
      // setSocketTimeout(socket.sockfd, TIMEOUT);
      PacketHeader ACKPacket;
      ACKPacket = receiveDataACK(socket, ACKPacket, logFile);

      // find the highest seqNum ACK
      if ((ACKPacket.seqNum > tracker.highestACKSeqNum) && (ACKPacket.type == 3)) {
        tracker.highestACKSeqNum = ACKPacket.seqNum;
        // check if all packets have been ACKed
        if ((tracker.highestACKSeqNum) == (tracker.unACKedPackets.size())) {
          sendLoop = false;
          break;
        }
      }
    }

    // put ACKed packets up to highest seqNum ACK into ACKedPackets
    for (uint32_t i = lastHighestSeqNum; i < tracker.highestACKSeqNum; i++) {
      tracker.ACKedPackets[i] = tracker.unACKedPackets[i];
    }

    // move window to lowest unACKED packet
    windowEnd = tracker.highestACKSeqNum + atoi(windowSize);
    windowBegin = tracker.highestACKSeqNum;
  } 

  return true;
}

int main(int argc, char* argv[]) 
{	
  // SENDER
  // ./rSender <receive-IP> <receiver-port> <window-size> <input-file> <log> 
  // retrieve inputted args
  args senderArgs = retrieveArgs(argv);

  // setup socket
  socketInfo socket = setupSocket(senderArgs.receiverPort, senderArgs.receiverIP);
  // set timeout to 500 ms
  setSocketTimeout(socket.sockfd, TIMEOUT);

  // send START, receive ACK
  PacketHeader STARTPacket = createSTARTPacket(); 
  if (sendSTART(socket, STARTPacket, senderArgs.log)) {
    PacketHeader ACKPacket;
    while(!getSTARTACK(socket, ACKPacket, senderArgs.log)) {
      sendSTART(socket, STARTPacket, senderArgs.log);
    }
  }

  packetTracker tracker = readFileInToTracker(senderArgs.inputFile);

  if (rUDPSend(socket, senderArgs.windowSize, tracker, senderArgs.log)) {
    // send END, receive ACK
    PacketHeader ENDPacket = createENDPacket(STARTPacket.seqNum);
    // set timeout to 500 ms
    // setSocketTimeout(socket.sockfd, TIMEOUT);
    if (sendEND(socket, ENDPacket, senderArgs.log)) {
      PacketHeader ACKPacket;
      while(!getENDACK(socket, ACKPacket, senderArgs.log)) {
        sendEND(socket, ENDPacket, senderArgs.log);
      }
    }
  }
  return 0;
}
