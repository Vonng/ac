upload:
    ssh 10.191.160.30 "rm -rf /ramdisk/vonng/vonng"
    scp /Users/vonng/Dev/go/src/ac/bin/vonng 10.191.160.30:/ramdisk/go/src/ac

remote:
	GOOS=linux GOARCH=amd64 go build -o ac.linux ac.go
	scp ac.linux 10.191.160.30:/ramdisk/ac.linux
	ssh 10.191.160.30 "GOGC=off time /ramdisk/ac.linux -i /ramdisk/video_title.txt -o /ramdisk/ac.txt"

remotetest:
	GOOS=linux GOARCH=amd64 go build -o ac.linux ac.go
	scp ac.linux 10.191.160.30:/ramdisk/ac.test
rt:
	ssh 10.191.160.30 "GOGC=off time /ramdisk/ac -i /ramdisk/video_title.txt -o /ramdisk/ac.txt"

upload:
	GOOS=linux GOARCH=amd64 go build -o ac.linux ac.go
	scp ac.linux tt:/tmp/ac
	ssh tt "rm /tmp/ac.txt && GOGC=off time /tmp/ac"
.PHONY: install clean upload build run check p sync
