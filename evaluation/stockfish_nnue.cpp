#include <chrono>
#include "stockfish_nnue.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <sstream>
#include <cstring>

namespace chess {

StockfishEvaluator& StockfishEvaluator::instance() {
    static StockfishEvaluator inst;
    return inst;
}

StockfishEvaluator::~StockfishEvaluator() {
    if (pid_>0) {
        send("quit");
        close(to_child_fd_);
        close(from_child_fd_);
        waitpid(pid_, nullptr, 0);
    }
}

bool StockfishEvaluator::init(const std::string& stockfish_path) {
    if (available_) return true;

    int to_child[2], from_child[2];
    if (pipe(to_child)!=0 || pipe(from_child)!=0) return false;

    pid_ = fork();
    if (pid_==0) {
        // child
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execl(stockfish_path.c_str(), stockfish_path.c_str(), nullptr);
        _exit(1);
    }
    // parent
    close(to_child[0]);
    close(from_child[1]);
    to_child_fd_ = to_child[1];
    from_child_fd_ = from_child[0];

    // Make non-blocking for reading with timeout
    int flags = fcntl(from_child_fd_, F_GETFL, 0);
    fcntl(from_child_fd_, F_SETFL, flags | O_NONBLOCK);

    // Send uci and wait for uciok
    send("uci");
    std::string resp = recv_until("uciok", 2000);
    if (resp.find("uciok")==std::string::npos) {
        return false;
    }
    send("isready");
    resp = recv_until("readyok", 2000);
    available_ = (resp.find("readyok")!=std::string::npos);
    return available_;
}

void StockfishEvaluator::send(const std::string& cmd) {
    if (to_child_fd_>=0) {
        std::string s = cmd + "\n";
        write(to_child_fd_, s.c_str(), s.size());
    }
}

std::string StockfishEvaluator::recv_until(const std::string& marker, int timeout_ms) {
    std::string buffer;
    char tmp[4096];
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now-start).count();
        if (elapsed > timeout_ms) break;
        struct pollfd pfd;
        pfd.fd = from_child_fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 100);
        if (ret>0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(from_child_fd_, tmp, sizeof(tmp)-1);
            if (n>0) {
                tmp[n]='\0';
                buffer+=tmp;
                if (buffer.find(marker)!=std::string::npos) break;
            }
        }
    }
    return buffer;
}

Score StockfishEvaluator::evaluate(const Position& pos) {
    if (!available_) return 0;
    // Send position and eval
    std::string fen = pos.fen();
    send("position fen " + fen);
    send("eval");
    std::string resp = recv_until("Final evaluation", 2000);
    // Parse "Final evaluation +0.15 (white side)" or similar
    // Look for line "Final evaluation"
    std::istringstream iss(resp);
    std::string line;
    Score result=0;
    while (std::getline(iss, line)) {
        if (line.find("Final evaluation")!=std::string::npos) {
            // Example: Final evaluation       +0.15 (white side) [with scaled NNUE, ...]
            // Extract number after "Final evaluation"
            size_t pos1 = line.find("Final evaluation");
            std::string sub = line.substr(pos1);
            // Find + or - number
            // Use sscanf
            double val=0;
            if (sscanf(sub.c_str(), "Final evaluation %lf", &val)==1) {
                result = static_cast<Score>(val*100); // convert pawn units to cp (0.15 pawn = 15 cp)
            } else {
                // Try to find +/- number
                size_t plus = sub.find("+");
                size_t minus = sub.find("-", sub.find("evaluation")+1);
                // Simple parse: look for +0.15 or -0.15
                // We'll try to extract first floating number after "Final evaluation"
                std::string tmp;
                bool inNum=false;
                for (char c: sub) {
                    if ((c>='0'&&c<='9')||c=='.'||c=='-'||c=='+') {
                        if (c=='-'||c=='+'|| (c>='0'&&c<='9') || c=='.') {
                            tmp+=c;
                            inNum=true;
                        }
                    } else {
                        if (inNum && tmp.size()>1) break;
                    }
                }
                try {
                    double v = std::stod(tmp);
                    result = static_cast<Score>(v*100);
                } catch(...) {
                    result=0;
                }
            }
            // Convert white side eval to side to move perspective
            if (pos.side_to_move()==Color::Black) result = -result;
            break;
        }
    }
    // Clamp to avoid mate scores
    if (result>10000) result=10000;
    if (result<-10000) result=-10000;
    return result;
}

} // namespace chess
