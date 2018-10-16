package main

import (
	"context"
	"net/http"

	"github.com/ethereum/go-ethereum/common/hexutil"
	infer "github.com/ethereum/go-ethereum/inference/synapse"
	"github.com/ethereum/go-ethereum/log"
)

func infoHashHandler(w http.ResponseWriter, inferWork *InferWork) {
	if inferWork.ModelHash == "" {
		RespErrorText(w, ErrModelEmpty)
		return
	}
	if inferWork.InputHash == "" {
		RespErrorText(w, ErrInputEmpty)
		return
	}

	log.Info("Infer Task", "Model Hash", inferWork.ModelHash, "Input Hash", inferWork.InputHash)
	label, err := infer.Engine().InferByInfoHash(inferWork.ModelHash, inferWork.InputHash)

	if err == nil {
		log.Info("Infer Success", "result", label)
	} else {
		log.Warn("Infer Failed", "error", err)
		RespErrorText(w, "Inference Error", "error", err, "model hash", inferWork.ModelHash, "input hash", inferWork.InputHash)
		return
	}

	RespInfoText(w, label)
}

func inputContentHandler(w http.ResponseWriter, inferWork *InferWork) {
	if inferWork.ModelHash == "" {
		RespErrorText(w, ErrModelEmpty)
		return
	}

	log.Info("Infer Work", "Model Hash", inferWork.ModelHash, "Input Address", inferWork.InputAddress, "Input Slot", inferWork.InputSlot)

	addr, slot := inferWork.InputAddress, inferWork.InputSlot

	cacheKey := inferWork.ModelHash + addr + slot
	if v, ok := simpleCache.Load(cacheKey); ok && !(*IsNotCache) {
		RespInfoText(w, v.(uint64))
		return
	}

	log.Debug("JSON-RPC request | ctx_getSolidityBytes", "address", addr, "slot", slot, "block number", "latest")
	var result hexutil.Bytes
	if rpcErr := rpcClient.CallContext(context.Background(), &result, "ctx_getSolidityBytes", addr, slot, "latest"); rpcErr != nil {
		log.Warn("JSON-RPC request failed", "error", rpcErr)
		RespErrorText(w, "JSON-RPC invoke ctx_getSolidityBytes", "error", rpcErr, "address", addr, "slot", slot)
		return
	}
	var inputArray = result[:]
	log.Debug("Infer Task By Input Content", "model info hash", inferWork.ModelHash, "input content", inputArray)
	label, err := infer.Engine().InferByInputContent(inferWork.ModelHash, inputArray)

	if err == nil {
		log.Info("Infer Result", "result", label)
	} else {
		log.Warn("Infer Failed", "error", err)
		RespErrorText(w, "Inference Error", "error", err, "model hash", inferWork.ModelHash, "address", addr, "slot", slot)
		return
	}

	if !(*IsNotCache) {
		simpleCache.Store(cacheKey, label)
	}

	RespInfoText(w, label)
}

func defaultHandler(w http.ResponseWriter, inferWork *InferWork) {
	RespErrorText(w, ErrInvalidInferTaskType, "type", inferWork.Type)
}