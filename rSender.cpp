#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstring>
#include <time.h>

#include "PacketHeader.h"
#include "crc32.h"

#define DATABUFFERSIZE 1465
#define PACKETBUFFERSIZE 1472

using namespace std; 
	
// Timeout timer
// 500 ms timeout 
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
  cout << "SENT START" << endl;
  return true;
}

bool getSTARTACK(socketInfo &socket, PacketHeader ACKPacket) {
  if (recvfrom(socket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr *) &socket.server_addr, &socket.server_len) == -1)
  {
    printf("Error receiving ACK for START\n");
    exit(1);
  }
  printf("Received ACK for START packet from %s:%d with SeqNum: %d\n", inet_ntoa(socket.server_addr.sin_addr), ntohs(socket.server_addr.sin_port), ACKPacket.seqNum);
  return true;
}

// void serialize(PacketHeader* header, char *data)
// {
//     int *q = (int*)data;    
//     *q = header->type;       q++;    
//     *q = header->seqNum;   q++;    
//     *q = header->length;     q++;
//     *q = header->checkSum;     q++;

//     char *p = (char*)q;
//     int i = 0;
//     while (i < 16)
//     {
//         *p = header->message[i];
//         p++;
//         i++;
//     }
// }

void sendData(socketInfo &socket, char* filePath) {

  // GET DATA FROM FILE TO SEND
  FILE* file = fopen(filePath, "r");
  //obtain file size
  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  rewind(file);
  //read file into buffer
  char data[DATABUFFERSIZE]; // don't know the type of file .. can't be auto
  int bytesRead = fread(data, 1, fileSize, file);

  // GET CHECKSUM
  uint32_t checkSum = crc32(data, bytesRead);
  cout << "CHECKSUM: " << checkSum << endl;

  // CREATE PACKET HEADER
  // PacketHeader header;
  // header.type = 2;
  // header.seqNum = 0; // initial 
  // header.length = bytesRead;
  // header.checksum = checkSum;
  // using dummy array for now
  char header[3];
  header[0] = '2';
  header[1] = '0';
  header[2] = '0';

  // add header to data
  char packet[PACKETBUFFERSIZE];
  memcpy(packet, header, sizeof(header));
  memcpy(packet + sizeof(header), data, bytesRead);
  // char* packet = new char[PACKETBUFFERSIZE];
  // memcpy(packet, &header, sizeof(header));
  // memcpy(packet + sizeof(header), data, bytesRead);

  cout << "Packet: " << packet[5] << endl;

  if (sendto(socket.sockfd, packet, sizeof(packet), 0, (struct sockaddr *) &socket.server_addr, socket.server_len) == -1) 
  {
    printf("Error sending data\n");
    exit(1);
  }


  cout << "SENT DATA" << endl;
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
    getSTARTACK(socket, ACKPacket);
  }

  sendData(socket, senderArgs.inputFile);

  // read input file, reading X amount of bytes (1472 for header and data)
	
  // append checksum to data (32-bit CRC header provided for checksums)

  // track sequence number, increment by 1 when sending

  // set sliding window (size is inputted arg)
  // window is the number of unACKED packets the sender can have in the network

  // recieve cumalitive ACKs from receiver 
  return 0;
}
