build:
	go build -o ac
	time ./ac

run:
	GOGC=off ./ac

runp:
	GOGC=off ./ac -p=profile

setup:
	util/ramdisk.sh && mkdir -p /ramdisk/vonng
	tar -xf data/dict.txt.tgz -C /ramdisk/vonng
	cat data/xa* | tar -x -C /ramdisk/vonng

prof:
	go tool pprof profile

rprof:
	scp 10.191.160.30:/ramdisk/go/src/ac/profile . &&  go tool pprof profile

clean:
	rm -rf  ac /tmp/ac.txt ac.darwin ac.linux profile bin/vonng /ramdisk/vonng/vonng.txt

sync:
    ssh 10.191.160.30 "rm -rf /ramdisk/go/src/ac" && \
    scp -r /Users/vonng/Dev/go/src/github.com/Vonng/ac 10.191.160.30:/ramdisk/go/src/ac

remote:
	GOOS=linux GOARCH=amd64 go build -o ac.linux ac.go
	scp ac.linux 10.191.160.30:/ramdisk/ac.linux
	ssh 10.191.160.30 "GOGC=off time /ramdisk/ac.linux -i /ramdisk/video_title.txt -o /ramdisk/ac.txt"

upload:
	GOOS=linux GOARCH=amd64 go build -o ac.linux ac.go
	scp ac.linux tt:/tmp/ac
	ssh tt "rm /tmp/ac.txt && GOGC=off time /tmp/ac"

.PHONY: build run runp setup prof rprof clean sync remote upload
