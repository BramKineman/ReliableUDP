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
  struct timeval timeout;
  // set timeout to 500 ms
  timeout.tv_sec = 0;
  timeout.tv_usec = 500000;
  if (setsockopt(newSocket.sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
    perror("Error setting timeout");
    exit(1);
  }

  return newSocket;
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

  // // create packet
  // packet dataPacket;
  // // clear buffer
  // memset(dataPacket.data,'\0', DATABUFFERSIZE);
  int seqNum = 0;
  // // create packet tracker
  // packetTracker* tracker = new packetTracker;

  // open file to read
  // ifstream file(filePath, ios::binary); // binary?
  // streamsize bytesRead;

  while(true) {
    // send all packets in window
    for (int i = 0; i < atoi(windowSize); i++) {
      if (tracker.unACKedPackets[i].seqNum == seqNum) {
        cout << endl << "Sending DATA: " << endl << tracker.unACKedPackets[i].data << endl << endl;
        // send packet
        if (sendto(socket.sockfd, (char*)&tracker.unACKedPackets[i], sizeof(tracker.unACKedPackets[i]) + HEADERSIZE, 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
        {
          printf("Error sending data\n");
          exit(1);
        }
      }
      seqNum++;
    }

    // break out of loop
    break;

    // read file
    // file.read(dataPacket.data, DATABUFFERSIZE);
    // bytesRead = file.gcount();
    // cout << "Read " << bytesRead << " bytes from file..." << endl;

    // create packet header
    // dataPacket.type = 2;
    // dataPacket.seqNum = seqNum; // initial
    // dataPacket.length = bytesRead;
    // dataPacket.checksum = crc32(dataPacket.data, bytesRead);
    //uint32_t totalPacketSize = HEADERSIZE + bytesRead;
    // cout << endl << "Sending DATA: " << endl << dataPacket.data << endl << endl;
    // cout << "Checksum: " << dataPacket.checksum << endl;

    // add packet to unacked tracker
    // tracker->unACKedPackets.insert({seqNum, dataPacket});
    // seqNum++;

    // send packet
    // if (sendto(socket.sockfd, &dataPacket, totalPacketSize, 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
    // {
    //   printf("Error sending data\n");
    //   exit(1);
    // }
    // cout << "Sent DATA... " << endl;  

    // collect any ACKs
    // PacketHeader ACKPacket = createHeader();
    // PacketHeader receivedACK = receiveDataACK(socket, ACKPacket);
    // if ACK is received, remove from unacked tracker
    // if (receivedACK.seqNum == dataPacket.seqNum) {
    //   tracker->unACKedPackets.erase(dataPacket.seqNum);
    // }
    // output ACK

    cout << "********************************************************" << endl;
  } 

  // close file
  // cout << "Closing file..." << endl;
  // file.close();
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
  // send START, receive ACK
  PacketHeader STARTPacket = createSTARTPacket(); 
  if (sendSTART(socket, STARTPacket)) {
    PacketHeader ACKPacket;
    while(!getSTARTACK(socket, ACKPacket)) {
      sendSTART(socket, STARTPacket);
    }
  }

  packetTracker tracker = readFileIntoTracker(senderArgs.inputFile);

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
