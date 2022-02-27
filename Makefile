all: rSender rReceiver

rSender: rSender.o
	g++ rSender.o -o rSender

rReceiver: rReceiver.o
	g++ rReceiver.o -o rReceiver

rSender.o: rSender.cpp
	g++ -c -Wall rSender.cpp

rReceiver.o: rReceiver.cpp
	g++ -c -Wall rReceiver.cpp

clean: 
	rm rSender.o rSender rReceiver.o rReceiver
