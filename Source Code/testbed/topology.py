#!/usr/bin/python

"""
changingtclink.py: Example of changing TCLink limits
This example runs 3 iperf measurements and plots the resulting
throughputs:
The first run resets the bandwidth limit hard.
The second run resets the bandwidth smooth.
The third run resets the bandwidth limit to 'no limit'.
"""

import re
import os
from time import sleep
import numpy as np
from mininet.net import Mininet
from mininet.link import TCIntf
from mininet.log import setLogLevel, info
from mininet.topo import Topo
from mininet.link import TCLink
from mininet.cli import CLI
#import matplotlib.pyplot as plt
import sys
TARGET_BW = 500
INITIAL_BW = 200
numClient = 20
 
print("\nArguments passed:")

numClient = int(sys.argv[1])


class StaticTopo(Topo):
    "Simple topo with 2 hosts"
    def build(self):
        switch1 = self.addSwitch('s1')
        switch2 = self.addSwitch('s2')

        "iperf server host"
        server = self.addHost('server', ip = '10.0.0.30')
        c = []
        for i in range(numClient):
            c.append(self.addHost('c'+str(i)))

        # = self.addHost('c10')

        # this link is not the bottleneck
        self.addLink(server, switch1) 

        "iperf client host"
        #host2 = self.addHost('h2')
        self.addLink(switch2, switch1, bw = 10, delay='10ms')
        for i in range(numClient):
            self.addLink(switch2, c[i])
       

def plotIperf(traces):
    for trace in traces:
        bw_list = []
        for line in open(trace[0], 'r'):
            matchObj = re.match(r'(.*),(.*),(.*),(.*),(.*),(.*),(.*),(.*),(.*)', line, re.M)
            
            if matchObj:
                bw = float(matchObj.group(9)) / 1000.0 / 1000.0 # MBit / s
                bw_list.append(bw)
        plt.plot(bw_list, label=trace[1])

    plt.legend()
    plt.title("Throughput Comparison")
    plt.ylabel("Throughput [MBit / s]")
    plt.xlabel("Time")
    plt.show()

def measureChange(h1, h2, smooth_change, output_file_name, target_bw = TARGET_BW):
    info( "Starting iperf Measurement\n" )

    # stop old iperf server
    os.system('pkill -f \'iperf -s\'')
    
    h1.cmd('iperf -s -i 0.5 -y C > ' + output_file_name + ' &')
    h2.cmd('iperf -c ' + str(h1.IP()) + ' -t 10 -i 1 > /dev/null &')

    # wait 5 seconds before changing
    sleep(5)

    intf = h2.intf()
    info( "Setting BW Limit for Interface " + str(intf) + " to " + str(target_bw) + "\n" )
    intf.config(bw = target_bw, smooth_change = smooth_change)

    # wait a few seconds to finish
    sleep(10)
    
def limit():
    """Example of changing the TCLinklimits"""
    myTopo = StaticTopo()
    
    fileName = 'timing'+str(numClient) +'.txt'
    net = Mininet( topo=myTopo, link=TCLink )
    net.start()
    timing = np.loadtxt(fileName)
    # for i in range(len(timing)):
    #     timing[i] = 0
    print (timing)
    server = net.get('server')
    c = []
    for i in range(numClient):
        c.append(net.get('c' + str(i)))
   
    # h2 = net.get('h2')
    # intf = h2.intf()
    info("Server is running....\n")
    server.cmd('cd testbed/Server/')
    server.cmd('./server_KPush>log.txt&')
    sleep(1)
    info("Client 1 joining\n")
    c[0].cmd ('cd nghttp2/src')
    c[0].cmd('./nghttp -snv http://10.0.0.30:3002/req_vod/bitrate=459/num=1>./'+str(numClient)+'/log0.txt&')
   
    # info("Client 2 joining\n")
    # c[1].cmd ('cd ../testbed/nghttp2/src')
    # c[1].cmd('nghttp -snv http://10.0.0.12:3002/req_vod/bitrate=459/num=1>log1.txt&')
    # sleep (timing[1] - timing[0])
    # info("Client 3 joining")
    # c[2].cmd ('cd ../testbed/nghttp2/src')
    # c[2].cmd('nghttp -snv http://10.0.0.12:3002/req_vod/bitrate=459/num=1>log2.txt&')

    # sleep (timing[2] - timing[1])
    # info("Client 4 joining")
    # c[3].cmd ('cd ../testbed/nghttp2/src')
    # c[3].cmd('nghttp -snv http://10.0.0.12:3002/req_vod/bitrate=459/num=1>log3.txt&')

    # sleep (timing[3] - timing[2])
    # info("Client 5 joining")
    # c[4].cmd ('cd ../testbed/nghttp2/src')
    # c[4].cmd('nghttp -snv http://10.0.0.12:3002/req_vod/bitrate=459/num=1>log4.txt')
    
    for i in range(1, numClient):
        if (i == 1):
            sleep(timing[0])
        else:
            sleep(timing[i - 1] - timing[i - 2] )
        info("Client " + str(i + 1) + " joining\n")
        c[i].cmd('cd nghttp2/src')
        if i < numClient - 1:
            mycmd = './nghttp -snv -w' + str(int(timing[i-1]*1000))+ ' http://10.0.0.30:3002/req_vod/bitrate=459/num=1>./'+str(numClient)+'/log' + str(i) + '.txt&'
            info(mycmd + '\n')
            c[i].cmd(mycmd)
        else:
            mycmd = './nghttp -snv -w' + str(int(timing[i-1]*1000)) +' http://10.0.0.30:3002/req_vod/bitrate=459/num=1>./'+str(numClient)+'/log'+str(i)+'.txt'
            info(mycmd + '\n')
            c[i].cmd(mycmd)


    # traces = [] 

    # filename = 'iperfServer_hard.log'
    # measureChange(h1, h2, False, filename)
    # traces.append((filename, 'Hard'))

    # # reset bw to initial value
    # intf.config(bw = INITIAL_BW)
    # filename = 'iperfServer_smooth.log'
    # measureChange(h1, h2, True, filename, target_bw = 800)
    # traces.append((filename, 'Smooth'))

    # # reset bw to initial value
    # intf.config(bw = INITIAL_BW)

    # filename = 'iperfServer_nolimit.log'
    # measureChange(h1, h2, False, filename, target_bw = None)
    # traces.append((filename, 'No limit'))
    #CLI(net)
    sleep(10)
    net.stop()

    #plotIperf(traces)

if __name__ == '__main__':
    setLogLevel( 'info' )
    limit()
