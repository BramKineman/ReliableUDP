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
  newArgs.portNum = argv[1];
  newArgs.windowSize = argv[2];
  newArgs.outputDir = argv[3];
  newArgs.log = argv[4];
  return newArgs;
}

PacketHeader createACKPacket(int seqNum){
  PacketHeader ACKHeader;
  ACKHeader.type = 3;
  ACKHeader.seqNum = seqNum; 
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
  newSocket.client_len = sizeof(newSocket.client_addr);
  return newSocket;
}

bool receivedSTART(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &STARTPacket) {
  cout << "Waiting for START..." << endl;
  recvfrom(serverSocket.sockfd, (char*)&STARTPacket, sizeof(STARTPacket), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);
  if (STARTPacket.type == 0) {
    printf("Received START packet with SeqNum: %d\n", STARTPacket.seqNum); 
    return true;
  }
}

bool sendACK(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, PacketHeader &ACKPacket) {
  if (sendto(serverSocket.sockfd, (char*)&ACKPacket, sizeof(ACKPacket), 0, (struct sockaddr*)&clientSocket.client_addr, clientSocket.client_len) == -1)
  {
      // get error message and print it
      perror("Error sending ACK");
      return false;
  }
  printf("Sent ACK packet with SeqNum: %d\n", ACKPacket.seqNum);
  return true;
}

// receive data from client
bool receiveData(serverSocketInfo &serverSocket, clientSocketInfo &clientSocket, char* filePath, char* windowSize, packetTracker &tracker) {
  // packet to receive data in to
  packet receivedPacket;
  int recv_len;
  bool recvLoop = true;
  int expectedSeqNum = 0;
  int windowBegin = 0;
  int windowEnd = atoi(windowSize);

  while (recvLoop) {

    // try to receive all packets in window
    // loop through window size
    for (int i = windowBegin; i < windowEnd; i++) {
      memset(receivedPacket.data,'\0', DATABUFFERSIZE);
      recv_len = recvfrom(serverSocket.sockfd, (char*)&receivedPacket, sizeof(receivedPacket), 0, (struct sockaddr *) &clientSocket.client_addr, &clientSocket.client_len);

      if (receivedPacket.type == 1) {
        cout << "Got END packet" << endl;
        recvLoop = false;
        break;
      }
      else if (receivedPacket.type == 2) {
        cout << endl << "Received packet with..." << endl;
        cout << "Type: " << receivedPacket.type << endl;
        cout << "seqNum: " << receivedPacket.seqNum << endl;
        cout << "checkSum: " << receivedPacket.checksum << endl;
        cout << "Data: " << endl << receivedPacket.data << endl;
        cout << "Length: " << recv_len << endl;

        // Calculate checksum on data
        uint32_t checksum = crc32(receivedPacket.data, recv_len - HEADERSIZE);

        // rUDP LOGIC
        // check if checksum is correct
        if (checksum == receivedPacket.checksum) {
          cout << "Checksum is correct..." << endl;
          // check if seqNum is correct
          if (expectedSeqNum == receivedPacket.seqNum) { // expected seqNum is correct
            // check highest seqNum from in-order received packets, and send ACK with seqNum + 1
            expectedSeqNum++;
            PacketHeader ACKPacket = createACKPacket(expectedSeqNum);
            // add packet to ACKedPackets
            tracker.ACKedPackets[receivedPacket.seqNum] = receivedPacket;
            sendACK(serverSocket, clientSocket, ACKPacket);
          } else if (receivedPacket.seqNum >= expectedSeqNum + atoi(windowSize)) {
            // drop packets that are greater than or equal to expectedSeqNum + windowSize
            cout << "Dropping packet with seqNum: " << receivedPacket.seqNum << endl;
          } else { // seqNum is not expected seqNum, ACK with expected seqNum
            cout << "Unexpected seqNum..." << endl;
            PacketHeader ACKPacket = createACKPacket(expectedSeqNum);
            sendACK(serverSocket, clientSocket, ACKPacket);
          }
        } else {
          cout << "Checksum failed, dropping packet..." << endl;
          continue;
        }

        // update windowbegin to highest seqNum in ACKedPackets
        if (tracker.ACKedPackets.size() > 0) {
          windowBegin = tracker.ACKedPackets.rbegin()->first;
        }
        windowEnd = windowBegin + atoi(windowSize);

        cout << "********************************************************" << endl;
      }
    }
  }

  return true;
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
  cout << "Hello from receiver" << endl;
  
  // retrieve inputted args
  args receiverArgs = retrieveArgs(argv);
  cout << "port: " << receiverArgs.portNum << endl;
  cout << "window: " << receiverArgs.windowSize << endl;
  cout << "outputDir: " << receiverArgs.outputDir << endl; // TODO: needs to be DIRECTORY, not FILE
  cout << "log: " << receiverArgs.log << endl;

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

  while (receivedSTART(serverSocket, clientSocket, STARTPacket)) {
    PacketHeader ACKPacketForSTARTEND = createACKPacket(STARTPacket.seqNum);
    bool ackSent = sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND);
    if (ackSent) {
      
      if (receiveData(serverSocket, clientSocket, receiverArgs.outputDir, receiverArgs.windowSize, tracker)) {
        // write data to file
        writeDataToFile(receiverArgs.outputDir, tracker, fileNum);
        // send ACK for END
        sendACK(serverSocket, clientSocket, ACKPacketForSTARTEND); 
      }
    } 
  }


}
