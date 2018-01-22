package lib

import (
	"io"
	"os"
	"bufio"
	"unsafe"
)

/**************************************************************\
*                          Constant                            *
\**************************************************************/
const (
	BufSize = 64 * 4096

	RootState = 1
	FailState = -1

	MaskBegin  = 0xFF000000
	MaskEnd    = 0x00FF0000
	MaskType   = 0x0000C000
	MaskLength = 0x00003FFF

	TypeMovie = 1
	TypeMusic = 2
	TypeBoth  = 3

	DictSize = 106264
	maxRune  = 260
	maxByte  = 384
)

/**************************************************************\
*                       Global variable                        *
\**************************************************************/
// Cache
var (
	Buf   [maxByte]byte // max 383
	Cache [maxRune]rune // max 257
	BSP   = 0           // Buf Stack Pointer
)

// states
var (
	Base  []int
	Check []int
	Fail  []int
	Info  []int
)

// WriteRune put rune to global buffer (extreme ver without err check)
func WriteRune(r rune) {
	switch i := uint32(r); {
	case i <= 127:
		Buf[BSP] = byte(r)
		BSP++
	case i <= 2047:
		Buf[BSP] = 0xC0 | byte(r>>6)
		BSP++
		Buf[BSP] = 0x80 | byte(r)&0x3F
		BSP++
	case i <= 65535:
		Buf[BSP] = 0xE0 | byte(r>>12)
		BSP++
		Buf[BSP] = 0x80 | byte(r>>6)&0x3F
		BSP++
		Buf[BSP] = 0x80 | byte(r)&0x3F
		BSP++
	default:
		Buf[BSP] = 0xF0 | byte(r>>18)
		BSP++
		Buf[BSP] = 0x80 | byte(r>>12)&0x3F
		BSP++
		Buf[BSP] = 0x80 | byte(r>>6)&0x3F
		BSP++
		Buf[BSP] = 0x80 | byte(r)&0x3F
		BSP++
	}
}

// WriteByType put write target string to buffer via match type
func WriteByType(match int) {
	Buf[BSP] = 45
	BSP++
	Buf[BSP] = 42
	BSP++
	switch (match & MaskType) >> 14 {
	case TypeMovie:
		WriteRune(30005)
		WriteRune(24433)
	case TypeMusic:
		WriteRune(38899)
		WriteRune(20048)
	case TypeBoth:
		WriteRune(38899)
		WriteRune(20048)
		WriteRune(38)
		WriteRune(30005)
		WriteRune(24433)
	}
	Buf[BSP] = 42
	BSP++
	Buf[BSP] = 45
	BSP++
}

/**************************************************************\
*                       Line Processor                         *
\**************************************************************/

// HandleLine take one line , process and write it
func HandleLine(input []byte) (output []byte) {
	var Matches [5]int
	var nMatch, match, rCursor, wCursor, info, mLength, mBegin int
	var overlap bool
	state := RootState

	// stage 1 : find all match
	for _, c := range *(*string)(unsafe.Pointer(&input)) {
	transfer:
		Cache[rCursor] = c

		// state transfer
		if t := state + int(c) + RootState; t < DictSize { // 219169484
			if state == Check[t] { // 53870341
				state = Base[t]
				goto match
			}
			if state == RootState { // 117354923
				state = RootState
				goto match
			}
		}
		// reach fail state
		state = Fail[state]
		goto transfer

	match:
		if info = Info[state]; info != 0 {
			// there's a match , check it out
			mLength = info & MaskLength
			match = (rCursor-mLength+1)<<24 | (rCursor << 16) | info

			// lastMatch.End < match.Begin : add new match
			if nMatch == 0 || ((Matches[nMatch-1]&MaskEnd)>>16 ) < (match&MaskBegin)>>24 {
				Matches[nMatch] = match
				match = 0
				nMatch ++
				goto done
			}

			// newMatch.Begin <= lastMatch.Begin : abandon old match
			overlap = false
			for ; nMatch > 0 && ((match&MaskBegin)>>24) <= ((Matches[nMatch-1])&MaskBegin)>>24; nMatch-- {
				overlap = true
			}
			if overlap {
				Matches[nMatch] = match
				match = 0
				nMatch++
				goto done
			}
			// miss: omit new match
		}

	done:
		rCursor ++
	}

	// stage 2 : replace all match
	if nMatch == 0 {
		// no match, write line back
		return input
	} else {
		// reset buf
		BSP = 0
		matchIndex := 0
		match = Matches[matchIndex]
		mBegin = (match & MaskBegin) >> 24
		mLength = match & MaskLength

		for wCursor = 0; wCursor < rCursor; {
			if wCursor < mBegin {
				WriteRune(Cache[wCursor])
				wCursor ++
				continue
			}
			if wCursor == mBegin {
				WriteByType(match)
				wCursor += match & MaskLength
				if matchIndex+1 < nMatch {
					matchIndex++
					match = Matches[matchIndex]
					mBegin = (match & MaskBegin) >> 24
					mLength = match & MaskLength
				}
				continue
			} else {
				WriteRune(Cache[wCursor])
				wCursor ++
			}
		}
		return Buf[:BSP]
	}
}

/**************************************************************\
*                          Driver                              *
\**************************************************************/
// Run program with given parameter
func Run(inputPath, outputPath, dictPath string) {
	// dict
	ac := FromFile(dictPath)
	Base = ac.Base
	Check = ac.Check
	Fail = ac.Failure
	Info = ac.Output

	// input
	inputFile, err := os.Open(inputPath)
	if err != nil {
		panic(err)
	}
	defer inputFile.Close()
	reader := bufio.NewReaderSize(inputFile, BufSize)

	// output
	outputFile, err := os.Create(outputPath)
	if err != nil {
		panic(err)
	}
	defer outputFile.Close()
	writer := bufio.NewWriterSize(outputFile, BufSize)

	// process loop
	var line []byte
	for err = nil; err != io.EOF; line, err = reader.ReadSlice('\n') {
		writer.Write(HandleLine(line))
	}
	writer.Flush()
}
