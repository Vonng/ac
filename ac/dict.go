package ac

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"sort"
)

/**************************************************************\
*                          Constant                            *
\**************************************************************/
const (
	ResizeDelta = 64

	StrMovie = "-*电影*-"    // 774
	StrMusic = "-*音乐*-"    // 334
	StrBoth  = "-*音乐&电影*-" // 6
)

// Aho-Corasick with DAT
type AC struct {
	Base    []int
	Check   []int
	Failure []int // failure status
	Output  []int // hold a word length & type
}

/**************************************************************\
*                       Build From Dick                        *
\**************************************************************/
// FromDict Read original dict file
func FromDict(dict map[string]int) *AC {
	// convert dict to []rune array
	keywords := make([][]rune, len(dict))
	i := 0
	for k, _ := range dict {
		keywords[i] = []rune(k)
		i += 1
	}
	m := new(Automation)
	m.Build(keywords)

	ac := AC{
		Base:    m.trie.Base,
		Check:   m.trie.Check,
		Failure: m.failure,
		Output:  make([]int, len(m.failure)),
	}

	// find longest match, fill length & type into output array
	for state, words := range m.output {
		var maxLength int
		var maxWord []rune
		for _, word := range words {
			if len(word) > maxLength {
				maxLength = len(word)
				maxWord = word
			}
		}
		wordType := dict[string(maxWord)]
		ac.Output[state] = (wordType << 14) | (maxLength & MaskLength)
	}
	return &ac
}

/**************************************************************\
*                       Load From File                         *
\**************************************************************/
func FromFile(filename string) *AC {
	dict := make(map[string]int, 1115)
	f, err := os.OpenFile(filename, os.O_RDONLY, 0660)
	if err != nil {
		panic(err)
	}
	r := bufio.NewReader(f)
	for {
		l, err := r.ReadBytes('\n')
		if err != nil {
			break
		}
		piece := bytes.Split(bytes.TrimSpace(l), []byte("\t"))
		key := string(piece[0])
		switch string(piece[1]) {
		case StrMovie:
			dict[key] = TypeMovie
		case StrMusic:
			dict[key] = TypeMusic
		case StrBoth:
			dict[key] = TypeBoth
		}
	}
	return FromDict(dict)
}

/**************************************************************\
*                      Raw AC Automation                       *
\**************************************************************/
type Automation struct {
	trie    *DoubleArrayTrie
	failure []int
	output  map[int]([][]rune)
}

// Build from keyword list
func (m *Automation) Build(keywords [][]rune) (err error) {
	if len(keywords) == 0 {
		return fmt.Errorf("empty keywords")
	}

	d := new(DAT)

	trie := new(LinkedListTrie)
	m.trie, trie, err = d.Build(keywords)
	if err != nil {
		return err
	}

	m.output = make(map[int]([][]rune), 0)
	for idx, val := range d.Output {
		m.output[idx] = append(m.output[idx], val)
	}

	queue := make([](*LinkedListTrieNode), 0)
	m.failure = make([]int, len(m.trie.Base))
	for _, c := range trie.Root.Children {
		m.failure[c.Base] = RootState
	}
	queue = append(queue, trie.Root.Children...)

	for {
		if len(queue) == 0 {
			break
		}

		node := queue[0]
		for _, n := range node.Children {
			if n.Base == FailState {
				continue
			}
			inState := m.failure[node.Base]
		set_state:
			outState := m.Transition(inState, n.Code-RootState)
			if outState == FailState {
				inState = m.failure[inState]
				goto set_state
			}
			if _, ok := m.output[outState]; ok != false {
				m.output[n.Base] = append(m.output[outState], m.output[n.Base]...)
			}
			m.failure[n.Base] = outState
		}
		queue = append(queue, node.Children...)
		queue = queue[1:]
	}

	return nil
}

func (m *Automation) Transition(inState int, input rune) (outState int) {
	if inState == FailState {
		return RootState
	}

	t := inState + int(input) + RootState
	if t >= len(m.trie.Base) {
		if inState == RootState {
			return RootState
		}
		return FailState
	}
	if inState == m.trie.Check[t] {
		return m.trie.Base[t]
	}

	if inState == RootState {
		return RootState
	}

	return FailState
}

/**************************************************************\
*                       Trie Tree Impl                         *
\**************************************************************/
// Linked List Trie
type LinkedListTrieNode struct {
	Code                            rune
	Depth, Left, Right, Index, Base int
	SubKey                          []rune
	Children                        [](*LinkedListTrieNode)
}

// Trie Tree
type LinkedListTrie struct {
	Root *LinkedListTrieNode
}

// Double Array Trie
type DoubleArrayTrie struct {
	Base  []int
	Check []int
}

type dartsKey []rune
type datKeySlice []dartsKey

/**************************************************************\
*                      Double Array Trie                       *
\**************************************************************/
type DAT struct {
	dat          *DoubleArrayTrie
	llt          *LinkedListTrie
	used         []bool
	nextCheckPos int
	key          datKeySlice
	Output       map[int]([]rune)
}

func (k datKeySlice) Len() int {
	return len(k)
}

func (k datKeySlice) Less(i, j int) bool {
	iKey, jKey := k[i], k[j]
	iLen, jLen := len(iKey), len(jKey)

	var pos int = 0
	for {
		if pos < iLen && pos < jLen {
			if iKey[pos] < jKey[pos] {
				return true
			} else if iKey[pos] > jKey[pos] {
				return false
			}
		} else {
			if iLen < jLen {
				return true
			} else {
				return false
			}
		}
		pos++
	}

	return false
}

func (k datKeySlice) Swap(i, j int) {
	k[i], k[j] = k[j], k[i]
}

func (d *DAT) Build(keywords [][]rune) (*DoubleArrayTrie, *LinkedListTrie, error) {
	if len(keywords) == 0 {
		return nil, nil, fmt.Errorf("empty keywords")
	}

	d.dat = new(DoubleArrayTrie)
	d.resize(ResizeDelta)

	for _, keyword := range keywords {
		var dk dartsKey = keyword
		d.key = append(d.key, dk)
	}
	sort.Sort(d.key)

	d.Output = make(map[int]([]rune), len(d.key))
	d.dat.Base[0] = RootState
	d.nextCheckPos = 0

	d.llt = new(LinkedListTrie)
	d.llt.Root = new(LinkedListTrieNode)
	d.llt.Root.Depth = 0
	d.llt.Root.Left = 0
	d.llt.Root.Right = len(keywords)
	d.llt.Root.SubKey = nil
	d.llt.Root.Index = 0

	siblings, err := d.fetch(d.llt.Root)
	if err != nil {
		return nil, nil, err
	}
	for idx, ns := range siblings {
		if ns.Code > 0 {
			siblings[idx].SubKey = append(d.llt.Root.SubKey, ns.Code-RootState)
		}
	}

	_, err = d.insert(siblings)
	if err != nil {
		return nil, nil, err
	}

	return d.dat, d.llt, nil
}

func (d *DAT) resize(size int) {
	d.dat.Base = append(d.dat.Base, make([]int, (size - len(d.dat.Base)))...)
	d.dat.Check = append(d.dat.Check, make([]int, (size - len(d.dat.Check)))...)

	d.used = append(d.used, make([]bool, (size - len(d.used)))...)
}

func (d *DAT) fetch(parent *LinkedListTrieNode) (siblings [](*LinkedListTrieNode), err error) {
	siblings = make([](*LinkedListTrieNode), 0, 2)

	var prev rune = 0

	for i := parent.Left; i < parent.Right; i++ {

		if len(d.key[i]) < parent.Depth {
			continue
		}

		tmp := d.key[i]

		var cur rune = 0
		if len(d.key[i]) != parent.Depth {
			cur = tmp[parent.Depth] + 1
		}

		if prev > cur {
			return nil, fmt.Errorf("fetch error")
		}

		if cur != prev || len(siblings) == 0 {
			var subKey []rune
			if cur != 0 {
				subKey = append(parent.SubKey, cur-RootState)
			} else {
				subKey = parent.SubKey
			}

			tmpNode := new(LinkedListTrieNode)
			tmpNode.Depth = parent.Depth + 1
			tmpNode.Code = cur
			tmpNode.Left = i
			tmpNode.SubKey = make([]rune, len(subKey))
			copy(tmpNode.SubKey, subKey)
			if len(siblings) != 0 {
				siblings[len(siblings)-1].Right = i
			}
			siblings = append(siblings, tmpNode)
			if len(parent.Children) != 0 {
				parent.Children[len(parent.Children)-1].Right = i
			}
			parent.Children = append(parent.Children, tmpNode)
		}

		prev = cur
	}

	if len(siblings) != 0 {
		siblings[len(siblings)-1].Right = parent.Right
	}
	if len(parent.Children) != 0 {
		parent.Children[len(siblings)-1].Right = parent.Right
	}

	//return siblings, nil
	return parent.Children, nil
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func (d *DAT) insert(siblings [](*LinkedListTrieNode)) (int, error) {
	var begin int = 0
	var pos int = max(int(siblings[0].Code)+1, d.nextCheckPos) - 1
	var nonZeroNum int = 0
	var first bool = false

	if len(d.dat.Base) <= pos {
		d.resize(pos + 1)
	}

	for {
	next:
		pos++

		if len(d.dat.Base) <= pos {
			d.resize(pos + 1)
		}

		if d.dat.Check[pos] > 0 {
			nonZeroNum++
			continue
		} else if !first {
			d.nextCheckPos = pos
			first = true
		}

		begin = pos - int(siblings[0].Code)
		if len(d.dat.Base) <= (begin + int(siblings[len(siblings)-1].Code)) {
			d.resize(begin + int(siblings[len(siblings)-1].Code) + ResizeDelta)
		}

		if d.used[begin] {
			continue
		}

		for i := 1; i < len(siblings); i++ {
			if 0 != d.dat.Check[begin+int(siblings[i].Code)] {
				goto next
			}
		}
		break

	}

	if float32(nonZeroNum)/float32(pos-d.nextCheckPos+1) >= 0.95 {
		d.nextCheckPos = pos
	}
	d.used[begin] = true

	for i := 0; i < len(siblings); i++ {
		d.dat.Check[begin+int(siblings[i].Code)] = begin
	}

	for i := 0; i < len(siblings); i++ {
		newSiblings, err := d.fetch(siblings[i])
		if err != nil {
			return -1, err
		}

		if len(newSiblings) == 0 {
			d.dat.Base[begin+int(siblings[i].Code)] = -siblings[i].Left - 1
			d.Output[begin+int(siblings[i].Code)] = siblings[i].SubKey
			siblings[i].Base = FailState
			siblings[i].Index = begin + int(siblings[i].Code)
		} else {
			h, err := d.insert(newSiblings)

			if err != nil {
				return -1, err
			}
			d.dat.Base[begin+int(siblings[i].Code)] = h
			siblings[i].Index = begin + int(siblings[i].Code)
			siblings[i].Base = h
		}
	}

	return begin, nil
}
