# HTTPStreaming
An adaptive streaming system with a HTTP server and multiple clients based on nghttp2

Network topology


![alt text](https://github.com/haidang1201/HTTPStreaming/blob/main/Topology.png)

Source code structure <br />
	--Server: server_KPush.cpp: create a HTTP server that can push video segment to the clients periodically. <br />
	--Clients: nghttp2: modified version of nghttp2 which is employed an adaptive algorithm to dynamically decide video qualities and send HTTP requests to the server <br />
	--Network: topology.py: create the network topology, simulate the bandwidth fluctuation.<br />


How to perform experiments?


1. Build nghttp2 following instructions in: https://nghttp2.org/documentation/package_README.html

2. Install mininet to simulate network topologies: http://mininet.org/download/

3. Compile server file in folder Server: g++ -o server_KPush server_KPush.cpp -lnghttp2_asio -lboost_system -std=c++11 -lssl -lcrypto -lpthread, then execute the obtained object file.


4. Run adaptive algorithms by:
	Copy the corresponding file (Conventional, BOLA, FESTIVE, PANDA, Proposed) to folder nghttp2/src
	Delete nghttp.cc file in the folder nghttp2/src
	Rename copied file as nghttp.cc
	Run "make" command
5. Run network topology by command:
	sudo python topology.py [n]

	with n as number of clients in the network.
6. Collect the bitrate and bandwidth data of each client in log files in folder nghttp2/src 
namely log0.txt, log1.txt and so on.