package main

import (
	"flag"
	"ac/lib"
)

const (
	defaultDictPath   = "/ramdisk/vonng/dict.txt"
	defaultInputPath  = "/ramdisk/vonng/video_title.txt"
	defaultOutputPath = "/ramdisk/vonng/vonng.txt"
)

// args
var (
	dictPath   string // dict path
	inputPath  string // input path
	outputPath string // output path
)

func main() {
	flag.StringVar(&inputPath, "i", defaultInputPath, "input filename")
	flag.StringVar(&outputPath, "o", defaultOutputPath, "output filename")
	flag.StringVar(&dictPath, "d", defaultDictPath, "dict filename")
	flag.Parse()
	lib.Run(inputPath, outputPath, dictPath)
}
