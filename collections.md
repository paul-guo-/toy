## Makefile

CENTOS_VER = $(shell cat /etc/redhat-release | sed 's/.*release\ //' | awk -F'.' '{print $$1}')

## Bash

# Get centos version.
CENTOS_VER=`cat /etc/redhat-release | sed 's/.*release\ //' | awk -F'.' '{print $1}'`

# traverse
list=`cat listfile`
for entry in $list; do
	echo $entry
done

while IFS= read -r entry
do                                                                              
	echo $entry
done < listfile

# Do not echo with a newline.
echo -n test

for i in `seq 1 10`; do
	echo $i
done

for ((i = 0; i < 10; i++)); do
	echo $i
done

