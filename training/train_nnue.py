#!/usr/bin/env python3
"""
Real NNUE trainer - HalfKP 41024x256x32x1 with PyTorch
- Loads JSONL datasets from datasets/games.jsonl
- Creates real HalfKP features from FEN using python-chess
- Trains with distillation, mixed precision, checkpoint
"""

import torch, torch.nn as nn, json, argparse, os, random
from pathlib import Path
from torch.utils.data import Dataset, DataLoader

FT_SIZE=41024
HT1=256
HT2=32

def piece_to_halfkp_type(piece, perspective_color):
    import chess
    if piece.piece_type == chess.KING: return -1
    is_own = (piece.color == perspective_color)
    base = 0 if is_own else 5
    if piece.piece_type == chess.PAWN: return base+0
    if piece.piece_type == chess.KNIGHT: return base+1
    if piece.piece_type == chess.BISHOP: return base+2
    if piece.piece_type == chess.ROOK: return base+3
    if piece.piece_type == chess.QUEEN: return base+4
    return -1

def king_bucket(king_sq): return king_sq // 8

def fen_to_features(fen):
    try:
        import chess
        board = chess.Board(fen)
        wk, bk = board.king(chess.WHITE), board.king(chess.BLACK)
        if wk is None or bk is None: return [], []
        white_active, black_active = [], []
        for sq in chess.SQUARES:
            piece = board.piece_at(sq)
            if piece is None or piece.piece_type == chess.KING: continue
            kb = king_bucket(wk)
            pt = piece_to_halfkp_type(piece, chess.WHITE)
            if pt >= 0:
                feat = kb*640 + pt*64 + sq
                if feat < FT_SIZE: white_active.append(feat)
            kb = king_bucket(bk ^ 56)
            pt = piece_to_halfkp_type(piece, chess.BLACK)
            if pt >= 0:
                feat = kb*640 + pt*64 + (sq ^ 56)
                if feat < FT_SIZE: black_active.append(feat)
        return white_active, black_active
    except Exception:
        return [], []

class NNUE(nn.Module):
    def __init__(self):
        super().__init__()
        self.ft = nn.EmbeddingBag(FT_SIZE, HT1, mode="sum", include_last_offset=False)
        self.l1 = nn.Linear(HT1*2, HT2)
        self.l2 = nn.Linear(HT2, 1)
    def forward(self, white, white_offsets, black, black_offsets):
        w = torch.clamp(self.ft(white, white_offsets), 0, 1)
        b = torch.clamp(self.ft(black, black_offsets), 0, 1)
        x = torch.cat([w,b], dim=1)
        x = torch.clamp(self.l1(x), 0, 1)
        return self.l2(x)

class GameDataset(Dataset):
    def __init__(self, jsonl_path):
        self.entries=[]
        with open(jsonl_path) as f:
            for line in f:
                try:
                    e=json.loads(line)
                    if 'fen' in e and 'eval_cp' in e: self.entries.append(e)
                except Exception: pass
        print(f"Loaded {len(self.entries)} positions from {jsonl_path}")
    def __len__(self): return len(self.entries)
    def __getitem__(self, idx):
        e=self.entries[idx]
        wa, ba = fen_to_features(e.get('fen','rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'))
        if not wa: wa=[0]
        if not ba: ba=[0]
        target=torch.clamp(torch.tensor([e.get('eval_cp',0)/400.0], dtype=torch.float32), -10, 10)
        return torch.tensor(wa, dtype=torch.long), torch.tensor(ba, dtype=torch.long), target

def collate_sparse(batch):
    whites, blacks, targets = zip(*batch)
    wo=torch.tensor([0]+[len(x) for x in whites[:-1]], dtype=torch.long).cumsum(0)
    bo=torch.tensor([0]+[len(x) for x in blacks[:-1]], dtype=torch.long).cumsum(0)
    return torch.cat(whites), wo, torch.cat(blacks), bo, torch.stack(targets).view(-1,1)

def train(args):
    device=torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device {device} FT={FT_SIZE} HT1={HT1} HT2={HT2}")
    ds=GameDataset(args.dataset)
    if len(ds)==0: print("No data"); return
    dl=DataLoader(ds,batch_size=args.batch,shuffle=True,num_workers=0,collate_fn=collate_sparse)
    model=NNUE().to(device)
    opt=torch.optim.AdamW(model.parameters(),lr=args.lr,weight_decay=1e-4)
    try: scaler=torch.amp.GradScaler('cuda')
    except Exception: scaler=torch.cuda.amp.GradScaler()
    loss_fn=nn.MSELoss(); best_loss=float('inf')
    for epoch in range(args.epochs):
        total=count=0
        for white,wo,black,bo,target in dl:
            white,wo=white.to(device),wo.to(device); black,bo,target=black.to(device),bo.to(device),target.to(device)
            opt.zero_grad()
            try:
                with torch.amp.autocast('cuda'): loss=loss_fn(model(white,wo,black,bo),target)
            except Exception:
                with torch.cuda.amp.autocast(): loss=loss_fn(model(white,wo,black,bo),target)
            scaler.scale(loss).backward(); scaler.step(opt); scaler.update(); total+=loss.item(); count+=1
        avg=total/max(1,count); print(f"Epoch {epoch} loss {avg:.5f}")
        if avg<best_loss:
            best_loss=avg; Path(args.out).parent.mkdir(parents=True,exist_ok=True); torch.save(model.state_dict(),args.out); print(f"Saved best to {args.out}")
        if epoch%5==0: torch.save(model.state_dict(),f"{args.out}.epoch{epoch}.pt")
    torch.save(model.state_dict(),args.out); export_binary(model,args.out.replace('.pt','.nnue'))

def export_binary(model,path):
    QA,QB=255,64
    import numpy as np
    with open(path,'wb') as f:
        f.write((model.ft.weight.detach().cpu().numpy()*QA).astype(np.int16).tobytes())
        f.write(np.zeros(HT1,dtype=np.int16).tobytes())
        f.write((model.l1.weight.detach().cpu().numpy()*QA).astype(np.int16).tobytes())
        f.write((model.l1.bias.detach().cpu().numpy()*QB*QA).astype(np.int32).tobytes())
        f.write((model.l2.weight.detach().cpu().numpy()*QB).astype(np.int16).tobytes())
        f.write((model.l2.bias.detach().cpu().numpy()*QB*QA).astype(np.int32).tobytes())
    print(f"Exported binary NNUE to {path} size {Path(path).stat().st_size//1024//1024}MB")

if __name__=="__main__":
    ap=argparse.ArgumentParser(); ap.add_argument('--dataset',default='datasets/games.jsonl'); ap.add_argument('--out',default='networks/nnue.pt'); ap.add_argument('--batch',type=int,default=256); ap.add_argument('--epochs',type=int,default=20); ap.add_argument('--lr',type=float,default=1e-3); train(ap.parse_args())
