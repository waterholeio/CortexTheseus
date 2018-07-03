package types

import (
	"encoding/json"
	"errors"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/rlp"
)

var (
	ErrorCodeLengthNotEnough = errors.New("Code length should be larger than 2")
	ErrorCodeTypeModelMeta   = errors.New("Model meta should start with 0x0001")
	ErrorCodeTypeInputMeta   = errors.New("Input meta should start with 0x0002")
	ErrorDecodeModelMeta     = errors.New("Model meta decode error")
	ErrorDecodeInputMeta     = errors.New("Input meta decode error")
)

//InferMeta include ModelMeta struct and InputMeta type
type InferMeta interface {
	TypeCode() []byte
	RawSize() uint64
	// Gas() uint64
	AuthorAddress() common.Address
}

type ModelMeta struct {
	Hash          common.Hash    `json:"Hash"`
	RawSize       uint64         `json:"RawSize"`
	InputShape    []uint64       `json:"InputShape"`
	OutputShape   []uint64       `json:"OutputShape"`
	Gas           uint64         `json:"Gas"`
	AuthorAddress common.Address `json:"AuthorAddress"`
}
type InputMeta struct {
	Hash          common.Hash    `json:"Hash"`
	RawSize       uint64         `json:"RawSize"`
	Shape         []uint64       `json:"Shape"`
	AuthorAddress common.Address `json:"AuthorAddress"`
}

func (mm *ModelMeta) EncodeJSON() (string, error) {
	data, err := json.Marshal(mm)
	return string(data), err
}
func (mm *ModelMeta) DecodeJSON(s string) error {
	err := json.Unmarshal([]byte(s), mm)
	return err
}
func (im *InputMeta) EncodeJSON() (string, error) {
	data, err := json.Marshal(im)
	return string(data), err
}
func (im *InputMeta) DecodeJSON(s string) error {
	err := json.Unmarshal([]byte(s), im)
	return err
}

func ParseModelMeta(code []byte) (*ModelMeta, error) {
	if len(code) < 2 {
		return nil, ErrorCodeLengthNotEnough
	}
	if !(code[0] == 0x0 && code[1] == 0x1) {
		return nil, ErrorCodeTypeModelMeta
	}
	var modelMeta ModelMeta
	err := rlp.DecodeBytes(code[2:], &modelMeta)
	if err != nil {
		return nil, err
	}
	return &modelMeta, nil
}

func ParseInputMeta(code []byte) (*InputMeta, error) {
	if len(code) < 2 {
		return nil, ErrorCodeLengthNotEnough
	}
	if !(code[0] == 0x0 && code[1] == 0x2) {
		return nil, ErrorCodeTypeInputMeta
	}
	var inputMeta InputMeta
	err := rlp.DecodeBytes(code[2:], &inputMeta)
	if err != nil {
		return nil, err
	}

	return &inputMeta, nil
}