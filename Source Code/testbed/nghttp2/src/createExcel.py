import re
import pandas as pd

import sys
 
# total arguments
n = len(sys.argv)

 
print("\nArguments passed:")
for i in range(1, n):
    print(sys.argv[i])
numOfClients = int(sys.argv[1])
method = int(sys.argv[2])
methodArr = ["Conventional", "FESTIVE", "PANDA","BOLA_U", "Proposed"]
writer = pd.ExcelWriter("/home/haidang/Documents/JointProject/results/"+str(numOfClients)+"/"+methodArr[method]+".xlsx")
for i in range(numOfClients):
	file = open("./" + str(numOfClients) + "/log"+str(i)+".txt")
	lines = file.readlines()
	lastlines = lines[-200:]
	title = lines[-201]
	title = re.split(r'\t+', title)
	data = []
	for line in lastlines:
		l = re.split(r'\t+', line)
		l = l[:-1]
		l = [float(element) for element in l]
		data.append(l)

	# df2 = pd.DataFrame(l)
	# df2.to_excel(writer, index = False, sheet_name = "Sheet1")
	#print (data)
	df1 = pd.DataFrame(data, columns = title)
	df1.astype('float').dtypes
	df1.to_excel(writer, index = False, sheet_name = "Sheet" + str(i+1))
writer.save()
writer.close()
