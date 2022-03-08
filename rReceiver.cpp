#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstring>
#include <string>
#include "PacketHeader.h"

#define PACKETBUFFERSIZE 1472

using namespace std; 
	
// Timeout timer - 500 ms timeout 
struct timer {
  chrono::time_point<chrono::system_clock> start, end;
  chrono::duration<double> elapsed;
};

// command line arguments 
struct args {
  char* portNum;
  char* windowSize;
  char* outputDir;
  char* log;
};

// server socket info for port binding
struct serverSocketInfo {
  int sockfd;
  struct sockaddr_in server_addr;
  socklen_t server_len;
};

// client socket info for receiving data
struct clientSocketInfo {
  struct sockaddr_in client_addr;
  socklen_t client_len;
};

struct packet : public PacketHeader {
  char data[PACKETBUFFERSIZE];
};

auto retrieveArgs(char* argv[])  {
  args newArgs;
  newArgs.portNum = argv[1];
  newArgs.windowSize = argv[2];
  newArgs.outputDir = argv[3];
  newArgs.log = argv[4];
  return newArgs;
}

PacketHeader createACKPacket(int seqNum){
  PacketHeader ACKHeader;
  ACKHeader.type = 3;
  ACKHeader.seqNum = seqNum; // set to receieved seqNum
  ACKHeader.length = 0; // 0 for ACKs
  ACKHeader.checksum = 0; // zero checksum for START/END/ACK packets
  return ACKHeader;
}

serverSocketInfo setupServerSocket(char* portnum) {
  serverSocketInfo newSocket;
  newSocket.sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (newSocket.sockfd < 0) {
    perror("ERROR creating socket");
    exit(1);
  }
  memset((char *) &newSocket.server_addr, 0, sizeof(newSocket.server_addr));
  newSocket.server_addr.sin_family = AF_INET;
  newSocket.server_addr.sin_addr.s_addr = INADDR_ANY;
  newSocket.server_addr.sin_port = htons(atoi(portnum));
  if (bind(newSocket.sockfd, (struct sockaddr *) &newSocket.server_addr, sizeof(newSocket.server_addr)) < 0) {
    perror("ERROR on binding");
    exit(1);
  }
  return newSocket;
}

clientSocketInfo setupClientSocket() {
  clientSocketInfo newSocket;
  socklen_t client_len = sizeof(newSocket.client_addr);
  return newSocket;
}

PacketHeader waitForSTART(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &STARTPacket) {
  int recv_len;
  bool recvLoop = true;
  cout << "Waiting for START..." << endl;

  while (recvLoop) {
    recv_len = recvfrom(serverSocket.sockfd, (char*)&STARTPacket, sizeof(STARTPacket), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
    if (STARTPacket.type == 0) {
      recvLoop = false;
    }
  }
  printf("Received START packet with SeqNum: %d\n", STARTPacket.seqNum);
  return STARTPacket;
}

bool sendACK(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &ACKPacket) {
  if (sendto(serverSocket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr*)&clientSocket.client_addr, clientSocket.client_len) == -1)
  {
      printf("Error sending ACK\n");
      exit(1);
  }
  printf("Sent ACK packet with SeqNum: %d\n", ACKPacket.seqNum);
  return true;
}

void deserialize(char *data, packet* packet)
{
    int *q = (int*)data;    
    packet->type = *q;       q++;    
    packet->seqNum = *q;   q++;    
    packet->length = *q;     q++;
    packet->checksum = *q;     q++;

    char *p = (char*)q;
    int i = 0;
    while (i < PACKETBUFFERSIZE)
    {
        packet->data[i] = *p;
        p++;
        i++;
    }
}

// receive data from client
bool receiveData(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, char* filePath) {
  char buf[PACKETBUFFERSIZE]; // TODO 
  int recv_len;
  bool recvLoop = true;
  packet* receivedPacket = new packet;


  while (recvLoop) {
    recv_len = recvfrom(serverSocket.sockfd, buf, sizeof(buf), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
    if (recv_len == -1) {
      // perror("ERROR on recvfrom");
      // exit(1);

    }
    // deserialize data
    deserialize(buf, receivedPacket);
    if (receivedPacket->type == 1) {   // while recieved data doesn't have packet header of type 1 (END)
      cout << "Got END packet" << endl;
      recvLoop = false;
    }
    else if (receivedPacket->type == 2) {
      cout << "Received packet with Type: " << receivedPacket->type << endl;
      // print data received
      printf("Received data: %s\n", receivedPacket->data);
      FILE *fp = fopen(filePath, "w");
      // (elements to be written, size of each element, number of elements, file pointer)
      fwrite(receivedPacket->data, sizeof(char), recv_len, fp);
      fclose(fp);
    }
  }

  // check seqNum of received packet
  // if seqNum is not expected seqNum, ACK with expected seqNum
  // if correct seqNum, check highest seqNum from in-order received packets, and send ACK with seqNum + 1
  // calculate checkSum 
  // if correct checkSum, send ACK
  return true;
}

int main(int argc, char* argv[]) 
{
  // RECEIVER
  // ./rReceiver <port-num> <window-size> <output-dir> <log>
  cout << "Hello from receiver" << endl;
  
  // retrieve inputted args
  args receiverArgs = retrieveArgs(argv);
  cout << "port: " << receiverArgs.portNum << endl;
  cout << "window: " << receiverArgs.windowSize << endl;
  cout << "outputDir: " << receiverArgs.outputDir << endl;
  cout << "log: " << receiverArgs.log << endl;

  // create server socket to send from
  serverSocketInfo serverSocket = setupServerSocket(receiverArgs.portNum);
  // create client socket to recv from
  clientSocketInfo clientSocket = setupClientSocket();
  

  // make socket non blocking
  fcntl(serverSocket.sockfd, F_SETFL, O_NONBLOCK);
  // Empty START packet to receive in to
  PacketHeader STARTPacket;
  STARTPacket = waitForSTART(serverSocket, clientSocket, STARTPacket);
  // send ACK for START
  if (STARTPacket.type == 0) {
    PacketHeader ACKPacket = createACKPacket(STARTPacket.seqNum);
    sendACK(serverSocket, clientSocket, ACKPacket);
  }

  if (receiveData(serverSocket, clientSocket, receiverArgs.outputDir)) {
    // send ACK for END
    // ACK for END
    PacketHeader ACKPacket = createACKPacket(STARTPacket.seqNum);
    sendACK(serverSocket, clientSocket, ACKPacket); 
  }

  // receive packet, calculate checksum
  // if checksum value != checksum value in header, don't send ACK

  // send cumulative ACK for each packet received 
  // cumulative ACK should contain seqNum it expects to receive next

  // 2 main scenarios when getting data
  // 1. if receiver is expecting seqNum N, but receives a different seqNum, it will reply with an ACK with seqNum = N
  // 2. if it receives a packed with seqNum = N, it will check the highest seqNum from in-order packets it has received and reply with a seqNum one greater
   
  return 0;
}
