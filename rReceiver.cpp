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
#include <fstream>
#include <map>

#include "PacketHeader.h"
#include "crc32.h"

#define DATABUFFERSIZE 1456
#define HEADERSIZE 16
#define TOTALHEADERSIZE 44

using namespace std; 

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
};

// client socket info for receiving data
struct clientSocketInfo {
  struct sockaddr_in client_addr;
  socklen_t client_len;
};

struct packet : public PacketHeader {
  char data[DATABUFFERSIZE];
};

struct packetTracker {
  // <seqNum, packet>
  map<int, packet> ACKedPackets;
  map<int, packet> bufferedPackets;
};

auto retrieveArgs(char* argv[]) {
  args newArgs;
  newArgs.portNum = argv[1];
  newArgs.windowSize = argv[2];
  newArgs.outputDir = argv[3];
  newArgs.log = argv[4];
  return newArgs;
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
  newSocket.client_len = sizeof(newSocket.client_addr);
  return newSocket;
}

PacketHeader createACKPacket(int seqNum){
  PacketHeader ACKHeader;
  ACKHeader.type = 3;
  ACKHeader.seqNum = seqNum; 
  ACKHeader.length = 0; // 0 for ACKs
  ACKHeader.checksum = 0; // zero checksum for START/END/ACK packets
  return ACKHeader;
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

PacketHeader receiveSTART(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &STARTPacket, char* logFilePath) {
  recvfrom(serverSocket.sockfd, (char*)&STARTPacket, sizeof(STARTPacket), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
  writeToLogFile(logFilePath, to_string(STARTPacket.type), to_string(STARTPacket.seqNum), to_string(STARTPacket.length), to_string(STARTPacket.checksum));
  return STARTPacket;
}

bool sendACK(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &ACKPacket, char* logFilePath) {
  if (sendto(serverSocket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr*)&clientSocket.client_addr, clientSocket.client_len) == -1)
  {
      return false;
  }
  writeToLogFile(logFilePath, to_string(ACKPacket.type), to_string(ACKPacket.seqNum), to_string(ACKPacket.length), to_string(ACKPacket.checksum));
  return true;
}

packet peekAtNextPacket(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket) {
  packet nextPacket;
  int errno;
  memset(nextPacket.data,'\0', DATABUFFERSIZE);
  recvfrom(serverSocket.sockfd, (char*)&nextPacket, sizeof(nextPacket), MSG_PEEK, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
  return nextPacket;
}

void ensureSTARTACKReceived(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &STARTPacket, PacketHeader &ACKPacketForSTARTEND, char* logFilePath) {
  bool ensureSTARTACKReceived = false;
  while (!ensureSTARTACKReceived) {
    // packet to receive data in to
    packet peekedPacket = peekAtNextPacket(serverSocket, clientSocket);
    if (peekedPacket.type == 2) { // next packet is data packet
      ensureSTARTACKReceived = true;
    } 
    else {
      // receieve new start and send another ACK
      receiveSTART(serverSocket, clientSocket, STARTPacket, logFilePath);
      sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND, logFilePath);
    }
  }
}

// receive data from client
bool rUDPReceive(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, char* filePath, char* windowSize, packetTracker &tracker, char* logFilePath) {
  // packet to receive data in to
  packet receivedPacket;
  // clear tracker variables
  tracker.ACKedPackets.clear();
  tracker.bufferedPackets.clear();
  // initializing variables
  int recv_len;
  bool recvLoop = true;
  uint32_t expectedSeqNum = 0;
  int windowBegin = 0;
  int windowEnd = atoi(windowSize);

  while (recvLoop) {
    // try to receive all packets in window
    // loop through window size
    for (int i = windowBegin; i < windowEnd; i++) {
      memset(receivedPacket.data,'\0', DATABUFFERSIZE);
      recv_len = recvfrom(serverSocket.sockfd, (char*)&receivedPacket, sizeof(receivedPacket), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
      // write to log file
      writeToLogFile(logFilePath, to_string(receivedPacket.type), to_string(receivedPacket.seqNum), to_string(receivedPacket.length), to_string(receivedPacket.checksum));

      if (receivedPacket.type == 0) {
        // ignore START packets
        continue;
      }
      else if (receivedPacket.type == 1) {
        recvLoop = false;
        break;
      }
      else if (receivedPacket.type == 2) {
        // Calculate checksum on data
        uint32_t checksum = crc32(receivedPacket.data, recv_len - HEADERSIZE);

        // rUDP LOGIC
        // check if checksum is correct
        if (checksum == receivedPacket.checksum) {
          // check if seqNum is correct
          if (expectedSeqNum == receivedPacket.seqNum) { // expected seqNum is correct
            // check highest seqNum from in-order received packets, and send ACK with seqNum + 1
            expectedSeqNum++;
            PacketHeader ACKPacket = createACKPacket(expectedSeqNum);
            // add packet to ACKedPackets
            tracker.ACKedPackets[receivedPacket.seqNum] = receivedPacket;
            sendACK(serverSocket, clientSocket, ACKPacket, logFilePath);
          } else if (receivedPacket.seqNum >= expectedSeqNum + atoi(windowSize)) {
            // drop packets that are greater than or equal to expectedSeqNum + windowSize
            continue;
          } else { // seqNum is not expected seqNum, ACK with expected seqNum
            // buffer packet
            tracker.bufferedPackets[receivedPacket.seqNum] = receivedPacket;
            PacketHeader ACKPacket = createACKPacket(expectedSeqNum);
            sendACK(serverSocket, clientSocket, ACKPacket, logFilePath);
          }
        } else { // drop packets where checksum fails
          continue;
        }

        // update windowbegin to highest seqNum in ACKedPackets
        if (tracker.ACKedPackets.size() > 0) {
          windowBegin = tracker.ACKedPackets.rbegin()->first + 1;
        }
        windowEnd = windowBegin + atoi(windowSize);
      }
    }
  }

  return true;
}

void putBufferedPacketsIntoACKedPackets(packetTracker &tracker) {
  for (auto it = tracker.bufferedPackets.begin(); it != tracker.bufferedPackets.end(); ++it) {
    tracker.ACKedPackets[it->first] = it->second;
  }
}

void writeDataToFile(char* filePath, packetTracker &tracker, int &fileNum) {
  // create string for full file path
  string fullFilePath = string(filePath) + "/File-" + to_string(fileNum) + ".out";
  ofstream file(fullFilePath, ios::binary | ios::out);
  for (auto it = tracker.ACKedPackets.begin(); it != tracker.ACKedPackets.end(); ++it) {
    file.write(it->second.data, it->second.length);
  }
  file.close();
  fileNum++;
}

int main(int argc, char* argv[]) 
{
  // RECEIVER
  // ./rReceiver <port-num> <window-size> <output-dir> <log>  
  // retrieve inputted args
  args receiverArgs = retrieveArgs(argv);

  // create server socket to send from
  serverSocketInfo serverSocket = setupServerSocket(receiverArgs.portNum);
  // create client socket to recv from
  clientSocketInfo clientSocket = setupClientSocket();

  // Empty START packet to receive in to
  PacketHeader STARTPacket;
  // Tracker for data packets
  packetTracker tracker;
  // file number to write to
  int fileNum = 0;

  while (true) {
    STARTPacket = receiveSTART(serverSocket, clientSocket, STARTPacket, receiverArgs.log);
    PacketHeader ACKPacketForSTARTEND = createACKPacket(STARTPacket.seqNum);
    while (STARTPacket.type == 1) { // Handle straggler END packet if END ACK was lost
      sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND, receiverArgs.log);
      STARTPacket = receiveSTART(serverSocket, clientSocket, STARTPacket, receiverArgs.log);
    }
    
    bool ackSent = sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND, receiverArgs.log);
    if (ackSent) {
      ensureSTARTACKReceived(serverSocket, clientSocket, STARTPacket, ACKPacketForSTARTEND, receiverArgs.log);

      if (rUDPReceive(serverSocket, clientSocket, receiverArgs.outputDir, receiverArgs.windowSize, tracker, receiverArgs.log)) {
        void putBufferedPacketsIntoACKedPackets(packetTracker tracker);
        writeDataToFile(receiverArgs.outputDir, tracker, fileNum);
      }

      // send ACK for END
      sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND, receiverArgs.log);
    } 
  }
}
