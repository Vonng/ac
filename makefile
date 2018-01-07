go:
	go build -o vonng
	GOGC=off time ./vonng

build:
	go build -o vonng
	time ./vonng

run:
	GOGC=off ./vonng

check:
	python check.py

clean:
	rm -rf  /tmp/vonng.txt
	rm -rf  vonng
	rm -rf  vonng.darwin
	rm -rf  vonng.linux
	rm -rf  profile
	rm -rf bin

pprof:
	go build -o vonng
	time ./vonng -p
	go tool pprof vonng profile

ben:
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng
	rm -f /tmp/vonng.txt && GOGC=off time ./vonng

remote:
	GOOS=linux GOARCH=amd64 go build -o vonng.linux vonng.go
	scp vonng.linux 10.191.160.30:/ramdisk/vonng.linux
	ssh 10.191.160.30 "GOGC=off time /ramdisk/vonng.linux -i /ramdisk/video_title.txt -o /ramdisk/vonng.txt"

remotetest:
	GOOS=linux GOARCH=amd64 go build -o vonng.linux vonng.go
	scp vonng.linux 10.191.160.30:/ramdisk/vonng.test
rt:
	ssh 10.191.160.30 "GOGC=off time /ramdisk/vonng -i /ramdisk/video_title.txt -o /ramdisk/vonng.txt"

upload:
	GOOS=linux GOARCH=amd64 go build -o vonng.linux vonng.go
	scp vonng.linux tt:/tmp/vonng
	ssh tt "rm /tmp/vonng.txt && GOGC=off time /tmp/vonng"

.PHONY: install clean upload
