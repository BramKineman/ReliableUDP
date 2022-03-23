#include <stdio.h>
#include <string.h>
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
  map<int, packet> packetsInWindow;
  int highestACKSeqNum = 0;
};

auto retrieveArgs(char* argv[])  {
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

PacketHeader createHeader() {
  PacketHeader header;
  return header;
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

bool sendSTART(socketInfo &socket, PacketHeader &startHeader) {
  if (sendto(socket.sockfd, (char*)&startHeader, sizeof(startHeader), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
  {
    printf("Error sending start\n");
    exit(1);
  }
  cout << "Sent START..." << endl;
  return true;
}

bool sendEND(socketInfo &socket, PacketHeader &endHeader) {
  if (sendto(socket.sockfd, (char*)&endHeader, sizeof(endHeader), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
  {
    printf("Error sending end\n");
    exit(1);
  }
  cout << "Sent END..." << endl;
  return true;
}

bool getSTARTACK(socketInfo &socket, PacketHeader ACKPacket) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len)) {
    if (ACKPacket.type == 3) {
    printf("Received ACK for START packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
    return true;
    }
  }

  return false;
}

bool getENDACK(socketInfo &socket, PacketHeader ACKPacket) {

  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len)) {
    if (ACKPacket.type == 3) {
    printf("Received ACK for END packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
    return true;
    }
  }
  
  return false;
}

packetTracker readFileIntoTracker(char* inputFile) {
  packetTracker tracker;
  ifstream file(inputFile, ios::binary);
  if (!file.is_open()) {
    cout << "Error opening file" << endl;
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

PacketHeader receiveDataACK(socketInfo &socket, PacketHeader ACKPacket) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len) == -1)
  {
    printf("No ACK to receive\n");
    // exit(1);
  } else {
    printf("Received ACK for packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
  }
  return ACKPacket;
}

bool sendData(socketInfo &socket, char* filePath, char* windowSize, packetTracker &tracker) {

  bool sendLoop = true;
  int seqNum = 0;
  int windowBegin = 0;
  int windowEnd = atoi(windowSize);

  while(sendLoop) {
    // send all packets in window
    for (int i = windowBegin; i < windowEnd; i++) {
      // Guard for window size larger than number of packets to send
      if (tracker.unACKedPackets.find(i) == tracker.unACKedPackets.end()) {
        cout << "Window size larger than number of packets to send..." << endl;
        sendLoop = false;
        break;
      }

      if (tracker.unACKedPackets[i].seqNum == seqNum) {
        cout << endl << "Sending DATA: " << endl << tracker.unACKedPackets[i].data << endl << endl;
        cout << "With SeqNum: " << tracker.unACKedPackets[i].seqNum << endl;

        // send packet
        if (i == 0) { // only have timeout on first packet in window
          setSocketTimeout(socket.sockfd, TIMEOUT);
          if (sendto(socket.sockfd, (char*)&tracker.unACKedPackets[i], tracker.unACKedPackets[i].length + HEADERSIZE, 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
          {
            printf("Error sending data\n");
            exit(1);
          }
          // set socket timeout to 0
          setSocketTimeout(socket.sockfd, 0);
        } else {
          if (sendto(socket.sockfd, (char*)&tracker.unACKedPackets[i], tracker.unACKedPackets[i].length + HEADERSIZE, 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
          {
            printf("Error sending data\n");
            exit(1);
          }
        }
      }
      seqNum++;
    }

    // collect all ACKs
    for (int i = 0; i < atoi(windowSize); i++) {
      PacketHeader ACKPacket;
      ACKPacket = receiveDataACK(socket, ACKPacket);
      // find the highest seqNum ACK
      if (ACKPacket.seqNum > tracker.highestACKSeqNum) {
        tracker.highestACKSeqNum = ACKPacket.seqNum;
        // check if all packets have been ACKed
        if ((tracker.highestACKSeqNum) == (tracker.unACKedPackets.size())) {
          cout << "All packets ACKed" << endl;
          sendLoop = false;
          break;
        }
      }
    }

    // put ACKed packets up to highest seqNum ACK into ACKedPackets
    for (int i = 0; i < tracker.highestACKSeqNum; i++) {
      tracker.ACKedPackets[i] = tracker.unACKedPackets[i];
    }

    // move window to lowest unACKED packet
    windowEnd = tracker.highestACKSeqNum + atoi(windowSize);
    windowBegin = tracker.highestACKSeqNum;

    cout << "********************************************************" << endl;
  } 

  return true;
}

int main(int argc, char* argv[]) 
{	
  // SENDER
  // ./rSender <receive-IP> <receiver-port> <window-size> <input-file> <log>
  cout << "Hello from sender" << endl;
 
  // retrieve inputted args
  args senderArgs = retrieveArgs(argv);
  cout << "receiverIP: " << senderArgs.receiverIP << endl;
  cout << "receiverPort: " << senderArgs.receiverPort << endl;
  cout << "windowSize: " << senderArgs.windowSize << endl;
  cout << "inputFile: " << senderArgs.inputFile << endl;
  cout << "log: " << senderArgs.log << endl;

  // setup socket
  socketInfo socket = setupSocket(senderArgs.receiverPort, senderArgs.receiverIP);
  // set timeout to 500 ms
  setSocketTimeout(socket.sockfd, TIMEOUT);
  // send START, receive ACK
  PacketHeader STARTPacket = createSTARTPacket(); 
  if (sendSTART(socket, STARTPacket)) {
    PacketHeader ACKPacket;
    while(!getSTARTACK(socket, ACKPacket)) {
      sendSTART(socket, STARTPacket);
    }
  }

  packetTracker tracker = readFileIntoTracker(senderArgs.inputFile);
  cout << "tracker.unACKedPackets size: " << tracker.unACKedPackets.size() << endl;

  if (sendData(socket, senderArgs.inputFile, senderArgs.windowSize, tracker)) {
    // send END, receive ACK
    PacketHeader ENDPacket = createENDPacket(STARTPacket.seqNum);
    if (sendEND(socket, ENDPacket)) {
      PacketHeader ACKPacket;
      while(!getENDACK(socket, ACKPacket)) {
        cout << "Retrying..." << endl;
        sendEND(socket, ENDPacket);
      }
    }
  }

  cout << "Ending program..." << endl;
  return 0;
}
