1. Build nghttp2 following instructions in: https://nghttp2.org/documentation/package_README.html

2. Compile server file in folder Server: g++ -o server_KPush server_KPush.cpp -lnghttp2_asio -lboost_system -std=c++11 -lssl -lcrypto -lpthread


3. Run an adaptive algorithm by:
	Copy the corresponding file (Conventional, BOLA, FESTIVE, PANDA, Proposed) to folder nghttp2/src
	Delete nghttp.cc file in the folder nghttp2/src
	Rename copied file as nghttp.cc
	Run "make" command
4. Run network topoloty by command:
	sudo python topology.py 3

	with 3 as number of clients and can be varied.
5. Collect the bitrate and bandwidth data of each client in log files in folder nghttp2/src 
namely log0.txt, log1.txt and so on.
