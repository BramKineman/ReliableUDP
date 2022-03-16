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
#include <deque>
#include <fstream>

#include "PacketHeader.h"
#include "crc32.h"

#define DATABUFFERSIZE 1456
#define PACKETBUFFERSIZE 1472

using namespace std; 
	
// Timeout timer - 500 ms timeout 
struct timer {
  chrono::time_point<chrono::system_clock> start, end;
  chrono::duration<double> elapsed;
};

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
  deque<packet> unACKedPackets;
  deque<packet> ACKedPackets;
  deque<packet> packetsInWindow;
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
  timer timeout;
  timeout.start = chrono::system_clock::now();

  bool recvLoop = true;
  int recv_len;
  while (recvLoop) {
    recv_len = recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len);
    timeout.end = chrono::system_clock::now();
    timeout.elapsed = timeout.end - timeout.start;
    if (timeout.elapsed.count() > 0.5) {
      cout << "TIMEOUT" << endl;
      return false;
    }
    if (ACKPacket.type == 3) {
      printf("Received ACK for START packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
      recvLoop = false;
    }
  }
  
  return true;
}

bool getENDACK(socketInfo &socket, PacketHeader ACKPacket) {
  timer timeout;
  timeout.start = chrono::system_clock::now();

  bool recvLoop = true;
  int recv_len;
  while (recvLoop) {
    recv_len = recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len);
    timeout.end = chrono::system_clock::now();
    timeout.elapsed = timeout.end - timeout.start;
    if (timeout.elapsed.count() > 0.5) {
      cout << "TIMEOUT" << endl;
      return false;
    }
    if (ACKPacket.type == 3) {
      printf("Received ACK for END packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
      recvLoop = false;
    }
  }
  
  return true;
}

bool receiveDataACK(socketInfo &socket, PacketHeader ACKPacket) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len) == -1)
  {
    printf("Error receiving ACK for DATA\n");
    exit(1);
  }
  printf("Received ACK for DATA packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
  return true;
}

void serialize(packet* packet, char *data)
{
    int *q = (int*)data;    
    *q = packet->type;       q++;    
    *q = packet->seqNum;   q++;    
    *q = packet->length;     q++;
    *q = packet->checksum;     q++;

    char *p = (char*)q;
    int i = 0;
    while (i < PACKETBUFFERSIZE)
    {
        *p = packet->data[i];
        p++;
        i++;
    }
}

bool sendData(socketInfo &socket, char* filePath, char* windowSize) {

  // create packet
  packet dataPacket;
  int seqNum = 0;
  // create packet tracker
  packetTracker* tracker = new packetTracker;

  // open file to read
  ifstream file(filePath, ios::binary); // binary?
  streamsize bytesRead;

  while(!file.eof()) {
    // clear buffer
    memset(dataPacket.data,'\0', DATABUFFERSIZE);

    // read file
    file.read(dataPacket.data, DATABUFFERSIZE);
    bytesRead = file.gcount();
    cout << "Read " << bytesRead << " bytes from file..." << endl;

    // create packet header
    dataPacket.type = 2;
    dataPacket.seqNum = seqNum; // initial
    seqNum++;
    dataPacket.length = bytesRead;
    dataPacket.checksum = crc32(dataPacket.data, bytesRead);
    cout << "Sending DATA packet... " << dataPacket.data << endl;

    // serialize packet
    // char packet[PACKETBUFFERSIZE];
    // serialize(dataPacket, packet); TODO: Serialization (with a bunch of null chars?) breaks the program

    // start timer when sending packet
    // timer timeout;
    // timeout.start = chrono::system_clock::now();
    
    // add packet to unacked tracker
    // tracker->unACKedPackets.push_back(*dataPacket);

    // send packet
    if (sendto(socket.sockfd, &dataPacket, sizeof(dataPacket), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
    {
      printf("Error sending data\n");
      exit(1);
    }

    cout << "Sent DATA... " << endl;
  } 

  // close file
  cout << "Closing file..." << endl;
  file.close();
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
  // make socket non blocking
  fcntl(socket.sockfd, F_SETFL, O_NONBLOCK);
  // send START, receive ACK
  PacketHeader STARTPacket = createSTARTPacket(); 
  if (sendSTART(socket, STARTPacket)) {
    PacketHeader ACKPacket;
    while(!getSTARTACK(socket, ACKPacket)) {
      cout << "Retrying..." << endl;
      sendSTART(socket, STARTPacket);
    }
  }

  if (sendData(socket, senderArgs.inputFile, senderArgs.windowSize)) {
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

 

  // read input file, reading X amount of bytes (1472 for header and data)
	
  // append checksum to data (32-bit CRC header provided for checksums)

  // track sequence number, increment by 1 when sending

  // set sliding window (size is inputted arg)
  // window is the number of unACKED packets the sender can have in the network

  // recieve cumalitive ACKs from receiver 

  cout << "Ending program..." << endl;
  return 0;
}
