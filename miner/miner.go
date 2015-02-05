/*
	This file is part of go-ethereum

	go-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	go-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with go-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @authors
 * 	Jeffrey Wilcke <i@jev.io>
 * @date 2014
 *
 */

package miner

import (
	"fmt"
	"math/big"
	"sort"

	"github.com/ethereum/c-ethash/go-ethash"
	"github.com/ethereum/go-ethereum/core"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	"github.com/ethereum/go-ethereum/eth"
	"github.com/ethereum/go-ethereum/ethutil"
	"github.com/ethereum/go-ethereum/event"
	"github.com/ethereum/go-ethereum/logger"
	"github.com/ethereum/go-ethereum/pow"
	"github.com/ethereum/go-ethereum/state"
)

var dx = []byte{0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42}
var dy = []byte{0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43}

const epochLen = uint64(1000)

func getSeed(chainMan *core.ChainManager, block *types.Block) (x []byte, y []byte) {
	if block.Number().Uint64() == 0 {
		return dx, dy
	} else if block.Number().Uint64()%epochLen == 0 {
		x, y = getSeed(chainMan, chainMan.GetBlock(block.ParentHash()))
		if (block.Number().Uint64()/epochLen)%2 > 0 {
			y = crypto.Sha3(append(y, block.ParentHash()...))
		} else {
			x = crypto.Sha3(append(x, block.ParentHash()...))
		}
		return x, y
	} else {
		return getSeed(chainMan, chainMan.GetBlock(block.ParentHash()))
	}
}

type LocalTx struct {
	To       []byte `json:"to"`
	Data     []byte `json:"data"`
	Gas      string `json:"gas"`
	GasPrice string `json:"gasPrice"`
	Value    string `json:"value"`
}

func (self *LocalTx) Sign(key []byte) *types.Transaction {
	return nil
}

var minerlogger = logger.NewLogger("MINER")

type Miner struct {
	eth    *eth.Ethereum
	events event.Subscription

	uncles    []*types.Header
	localTxs  map[int]*LocalTx
	localTxId int

	pow       pow.PoW
	quitCh    chan struct{}
	powQuitCh chan struct{}

	Coinbase []byte

	mining bool

	MinAcceptedGasPrice *big.Int
	Extra               string
}

func New(coinbase []byte, eth *eth.Ethereum) *Miner {
	return &Miner{
		eth:                 eth,
		powQuitCh:           make(chan struct{}),
		mining:              false,
		localTxs:            make(map[int]*LocalTx),
		MinAcceptedGasPrice: big.NewInt(10000000000000),
		Coinbase:            coinbase,
	}
}

func (self *Miner) GetPow() pow.PoW {
	return self.pow
}

func (self *Miner) AddLocalTx(tx *LocalTx) int {
	minerlogger.Infof("Added local tx (%x %v / %v)\n", tx.To[0:4], tx.GasPrice, tx.Value)

	self.localTxId++
	self.localTxs[self.localTxId] = tx
	self.eth.EventMux().Post(tx)

	return self.localTxId
}

func (self *Miner) RemoveLocalTx(id int) {
	if tx := self.localTxs[id]; tx != nil {
		minerlogger.Infof("Removed local tx (%x %v / %v)\n", tx.To[0:4], tx.GasPrice, tx.Value)
	}
	self.eth.EventMux().Post(&LocalTx{})

	delete(self.localTxs, id)
}

func (self *Miner) Start() {
	if self.mining {
		return
	}

	minerlogger.Infoln("Starting mining operations")
	self.mining = true
	self.quitCh = make(chan struct{})
	self.powQuitCh = make(chan struct{})

	mux := self.eth.EventMux()
	self.events = mux.Subscribe(core.NewBlockEvent{}, core.TxPreEvent{}, &LocalTx{})

	go self.update()
	go self.mine()
}

func (self *Miner) Stop() {
	if !self.mining {
		return
	}

	self.mining = false

	minerlogger.Infoln("Stopping mining operations")

	self.events.Unsubscribe()

	close(self.quitCh)
	close(self.powQuitCh)
}

func (self *Miner) Mining() bool {
	return self.mining
}

func (self *Miner) update() {
out:
	for {
		select {
		case event := <-self.events.Chan():
			switch event := event.(type) {
			case core.NewBlockEvent:
				block := event.Block
				if self.eth.ChainManager().HasBlock(block.Hash()) {
					self.reset()
					self.eth.TxPool().RemoveSet(block.Transactions())
					go self.mine()
				} else if true {
					// do uncle stuff
				}
			case core.TxPreEvent, *LocalTx:
				self.reset()
				go self.mine()
			}
		case <-self.quitCh:
			break out
		}
	}
}

func (self *Miner) reset() {
	close(self.powQuitCh)
	self.powQuitCh = make(chan struct{})
}

func (self *Miner) mine() {
	var (
		blockProcessor = self.eth.BlockProcessor()
		chainMan       = self.eth.ChainManager()
		block          = chainMan.NewBlock(self.Coinbase)
		state          = state.New(block.Root(), self.eth.Db())
	)
	block.Header().Extra = self.Extra

	// Apply uncles
	if len(self.uncles) > 0 {
		block.SetUncles(self.uncles)
	}

	parent := chainMan.GetBlock(block.ParentHash())
	coinbase := state.GetOrNewStateObject(block.Coinbase())
	coinbase.SetGasPool(core.CalcGasLimit(parent, block))

	transactions := self.finiliseTxs()

	// Accumulate all valid transactions and apply them to the new state
	// Error may be ignored. It's not important during mining
	receipts, txs, _, erroneous, err := blockProcessor.ApplyTransactions(coinbase, state, block, transactions, true)
	if err != nil {
		minerlogger.Debugln(err)
	}
	self.eth.TxPool().RemoveSet(erroneous)

	block.SetTransactions(txs)
	block.SetReceipts(receipts)

	// Accumulate the rewards included for this block
	blockProcessor.AccumelateRewards(state, block, parent)

	state.Update(ethutil.Big0)
	block.SetRoot(state.Root())

	minerlogger.Infof("Mining on block. Includes %v transactions", len(transactions))

	x, y := getSeed(chainMan, block)
	self.pow, err = ethash.New(append(x, y...), block)
	if err != nil {
		fmt.Println("err", err)
		return
	}

	// Find a valid nonce
	nonce := self.pow.Search(block, self.powQuitCh)
	if nonce != nil {
		block.Header().Nonce = nonce
		err := chainMan.InsertChain(types.Blocks{block})
		if err != nil {
			minerlogger.Infoln(err)
		} else {
			self.eth.EventMux().Post(core.NewMinedBlockEvent{block})

			minerlogger.Infof("🔨  Mined block %x\n", block.Hash())
			minerlogger.Infoln(block)
		}

		go self.mine()
	}
}

func (self *Miner) finiliseTxs() types.Transactions {
	// Sort the transactions by nonce in case of odd network propagation
	actualSize := len(self.localTxs) // See copy below
	txs := make(types.Transactions, actualSize+self.eth.TxPool().Size())

	state := self.eth.ChainManager().TransState()
	// XXX This has to change. Coinbase is, for new, same as key.
	key := self.eth.KeyManager()
	for i, ltx := range self.localTxs {
		tx := types.NewTransactionMessage(ltx.To, ethutil.Big(ltx.Value), ethutil.Big(ltx.Gas), ethutil.Big(ltx.GasPrice), ltx.Data)
		tx.SetNonce(state.GetNonce(self.Coinbase))
		state.SetNonce(self.Coinbase, tx.Nonce()+1)

		tx.Sign(key.PrivateKey())

		txs[i] = tx
	}

	// Faster than append
	for _, tx := range self.eth.TxPool().GetTransactions() {
		if tx.GasPrice().Cmp(self.MinAcceptedGasPrice) >= 0 {
			txs[actualSize] = tx
			actualSize++
		}
	}

	newTransactions := make(types.Transactions, actualSize)
	copy(newTransactions, txs[:actualSize])
	sort.Sort(types.TxByNonce{newTransactions})

	return newTransactions
}
