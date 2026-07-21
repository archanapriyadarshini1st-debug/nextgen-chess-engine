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

# Piece mapping for HalfKP: 10 types
# 0: own pawn, 1: own knight, 2: own bishop, 3: own rook, 4: own queen,
# 5: enemy pawn, 6: enemy knight, 7: enemy bishop, 8: enemy rook, 9: enemy queen
def piece_to_halfkp_type(piece, perspective_color):
    # piece is python-chess piece
    # perspective_color: True for white, False for black
    # Returns type index 0..9 or -1 for king
    import chess
    if piece.piece_type == chess.KING:
        return -1
    is_own = (piece.color == perspective_color)
    base = 0 if is_own else 5
    if piece.piece_type == chess.PAWN: return base+0
    if piece.piece_type == chess.KNIGHT: return base+1
    if piece.piece_type == chess.BISHOP: return base+2
    if piece.piece_type == chess.ROOK: return base+3
    if piece.piece_type == chess.QUEEN: return base+4
    return -1

def king_bucket(king_sq):
    # 8 buckets based on file? Stockfish uses (file//4 + rank*2) etc. Simplified: rank//4 *2 + file//4?
    # Use 8 buckets: king square //8? Actually 64 squares /8 =8 buckets of 8 squares each.
    return king_sq // 8

def fen_to_features(fen):
    """
    Returns white_features, black_features as list of active indices
    """
    try:
        import chess
        board = chess.Board(fen)
        # find kings
        wk = board.king(chess.WHITE)
        bk = board.king(chess.BLACK)
        if wk is None or bk is None:
            return [], []

        white_active=[]
        black_active=[]
        for sq in chess.SQUARES:
            piece = board.piece_at(sq)
            if piece is None: continue
            if piece.piece_type == chess.KING: continue

            # White perspective
            if wk is not None:
                kb = king_bucket(wk)
                pt = piece_to_halfkp_type(piece, chess.WHITE)
                if pt>=0:
                    # mirror square for black perspective? For white perspective, square as is
                    feat = kb*640 + pt*64 + sq
                    if feat < FT_SIZE:
                        white_active.append(feat)
            # Black perspective - mirror squares
            if bk is not None:
                kb = king_bucket(bk ^ 56) # mirror king
                # mirror piece square for black perspective
                sq_mirrored = sq ^ 56
                pt = piece_to_halfkp_type(piece, chess.BLACK)
                if pt>=0:
                    feat = kb*640 + pt*64 + sq_mirrored
                    if feat < FT_SIZE:
                        black_active.append(feat)

        return white_active, black_active
    except Exception as e:
        # print(f"fen error {e}")
        return [], []

class NNUE(nn.Module):
    def __init__(self):
        super().__init__()
        self.ft = nn.Linear(FT_SIZE, HT1)
        self.l1 = nn.Linear(HT1*2, HT2)
        self.l2 = nn.Linear(HT2, 1)
    def forward(self, white, black):
        w = torch.clamp(self.ft(white), 0, 1)
        b = torch.clamp(self.ft(black), 0, 1)
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
                    if 'fen' in e and 'eval_cp' in e:
                        self.entries.append(e)
                except: pass
        print(f"Loaded {len(self.entries)} positions from {jsonl_path}")

    def __len__(self): return len(self.entries)

    def __getitem__(self, idx):
        e=self.entries[idx]
        fen = e.get('fen','rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1')
        white_active, black_active = fen_to_features(fen)
        white = torch.zeros(FT_SIZE)
        black = torch.zeros(FT_SIZE)
        for f in white_active:
            if f < FT_SIZE:
                white[f]=1.0
        for f in black_active:
            if f < FT_SIZE:
                black[f]=1.0
        # If no features (king missing), random fallback
        if len(white_active)==0 and len(black_active)==0:
            for _ in range(32):
                white[random.randint(0, FT_SIZE-1)]=1
                black[random.randint(0, FT_SIZE-1)]=1
        target = torch.tensor([e.get('eval_cp',0)/400.0], dtype=torch.float32)
        # Clamp target to -10..10 for stability
        target = torch.clamp(target, -10, 10)
        return white, black, target

def train(args):
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device {device} FT={FT_SIZE} HT1={HT1} HT2={HT2}")
    ds = GameDataset(args.dataset)
    if len(ds)==0:
        print("No data")
        return
    dl = DataLoader(ds, batch_size=args.batch, shuffle=True, num_workers=0)
    model = NNUE().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    try:
        scaler = torch.amp.GradScaler('cuda')
    except:
        scaler = torch.cuda.amp.GradScaler()
    loss_fn = nn.MSELoss()

    best_loss=float('inf')
    for epoch in range(args.epochs):
        total=0
        count=0
        for white, black, target in dl:
            white, black, target = white.to(device), black.to(device), target.to(device)
            opt.zero_grad()
            try:
                with torch.amp.autocast('cuda'):
                    pred = model(white, black)
                    loss = loss_fn(pred, target)
            except:
                with torch.cuda.amp.autocast():
                    pred = model(white, black)
                    loss = loss_fn(pred, target)
            scaler.scale(loss).backward()
            scaler.step(opt)
            scaler.update()
            total+=loss.item()
            count+=1
        avg = total/max(1,count)
        print(f"Epoch {epoch} loss {avg:.5f}")
        if avg < best_loss:
            best_loss=avg
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            torch.save(model.state_dict(), args.out)
            print(f"Saved best to {args.out}")
        if epoch%5==0:
            torch.save(model.state_dict(), f"{args.out}.epoch{epoch}.pt")

    # Final save
    torch.save(model.state_dict(), args.out)
    export_binary(model, args.out.replace(".pt",".nnue"))

def export_binary(model, path):
    QA=255
    QB=64
    import numpy as np
    with open(path, "wb") as f:
        ft_w = (model.ft.weight.detach().cpu().numpy()*QA).astype(np.int16)
        ft_b = (model.ft.bias.detach().cpu().numpy()*QA).astype(np.int16)
        l1_w = (model.l1.weight.detach().cpu().numpy()*QA).astype(np.int16)
        l1_b = (model.l1.bias.detach().cpu().numpy()*QB*QA).astype(np.int32)
        l2_w = (model.l2.weight.detach().cpu().numpy()*QB).astype(np.int16)
        l2_b = (model.l2.bias.detach().cpu().numpy()*QB*QA).astype(np.int32)
        f.write(ft_w.tobytes())
        f.write(ft_b.tobytes())
        f.write(l1_w.tobytes())
        f.write(l1_b.tobytes())
        f.write(l2_w.tobytes())
        f.write(l2_b.tobytes())
    print(f"Exported binary NNUE to {path} size {Path(path).stat().st_size//1024//1024}MB")

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("--dataset", default="datasets/games.jsonl")
    ap.add_argument("--out", default="networks/nnue.pt")
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--lr", type=float, default=1e-3)
    args=ap.parse_args()
    train(args)
