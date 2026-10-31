using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;

internal static class NextGenNativeHost
{
    private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();

    private static bool ReadExact(Stream input, byte[] buffer, int count)
    {
        int offset = 0;
        while (offset < count)
        {
            int read = input.Read(buffer, offset, count - offset);
            if (read <= 0) return false;
            offset += read;
        }
        return true;
    }

    private static void Send(Stream output, object response)
    {
        byte[] payload = Encoding.UTF8.GetBytes(Json.Serialize(response));
        byte[] header = BitConverter.GetBytes(payload.Length);
        output.Write(header, 0, header.Length);
        output.Write(payload, 0, payload.Length);
        output.Flush();
    }

    private static string RepoRoot()
    {
        DirectoryInfo hostDir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
        DirectoryInfo extensionDir = hostDir.Parent;
        if (extensionDir == null || extensionDir.Parent == null)
            throw new InvalidOperationException("Could not locate the NextGen repository root.");
        return extensionDir.Parent.FullName;
    }

    private static string EnginePath(string root)
    {
        string[] candidates = new string[] {
            Path.Combine(root, "build", "Release", "chess_engine.exe"),
            Path.Combine(root, "build", "chess_engine.exe"),
            Path.Combine(root, "build", "Debug", "chess_engine.exe")
        };
        foreach (string candidate in candidates)
            if (File.Exists(candidate)) return candidate;
        throw new FileNotFoundException("NextGen engine executable not found. Run install-native-host.ps1 to build it.");
    }

    private static object Analyse(Dictionary<string, object> request)
    {
        string fen = Convert.ToString(request.ContainsKey("fen") ? request["fen"] : "");
        if (String.IsNullOrWhiteSpace(fen) || fen.Length > 200 ||
            !Regex.IsMatch(fen, @"^[prnbqkPRNBQK1-8/ ]+[wb] [KQkq-]+ [a-h36-]+ \d+ \d+$"))
            throw new ArgumentException("The FEN format is invalid.");

        int depth = 12;
        if (request.ContainsKey("depth"))
            Int32.TryParse(Convert.ToString(request["depth"]), out depth);
        depth = Math.Max(1, Math.Min(24, depth));

        bool useNnue = request.ContainsKey("useNnue") && Convert.ToBoolean(request["useNnue"]);
        string root = RepoRoot();
        string engine = EnginePath(root);
        string nnue = Path.Combine(root, "networks", "nnue.nnue");
        if (useNnue && !File.Exists(nnue))
            throw new FileNotFoundException("Experimental NNUE network was not found in networks/nnue.nnue.");

        ProcessStartInfo start = new ProcessStartInfo();
        start.FileName = engine;
        start.WorkingDirectory = root;
        start.UseShellExecute = false;
        start.CreateNoWindow = true;
        start.RedirectStandardInput = true;
        start.RedirectStandardOutput = true;

        using (Process process = new Process())
        {
            process.StartInfo = start;
            if (!process.Start()) throw new InvalidOperationException("Could not start NextGen engine.");
            using (Timer watchdog = new Timer(delegate {
                try { if (!process.HasExited) process.Kill(); } catch { }
            }, null, 120000, Timeout.Infinite))
            {
                string line;
                process.StandardInput.AutoFlush = true;
                process.StandardInput.WriteLine("uci");
                bool uciReady = false;
                while ((line = process.StandardOutput.ReadLine()) != null)
                    if (line.Trim() == "uciok") { uciReady = true; break; }
                if (!uciReady) throw new InvalidOperationException("Engine did not complete UCI handshake.");

                process.StandardInput.WriteLine("setoption name Hash value 64");
                process.StandardInput.WriteLine("setoption name Threads value 1");
                process.StandardInput.WriteLine("setoption name OwnBook value false");
                if (useNnue)
                    process.StandardInput.WriteLine("setoption name NNUEFile value " + nnue);
                process.StandardInput.WriteLine("isready");

                bool ready = false;
                while ((line = process.StandardOutput.ReadLine()) != null)
                    if (line.Trim() == "readyok") { ready = true; break; }
                if (!ready) throw new InvalidOperationException("Engine did not become ready.");

                process.StandardInput.WriteLine("position fen " + fen);
                process.StandardInput.WriteLine("go depth " + depth);

                string bestMove = null;
                string lastInfo = "";
                while ((line = process.StandardOutput.ReadLine()) != null)
                {
                    if (line.StartsWith("info ")) lastInfo = line;
                    if (line.StartsWith("bestmove "))
                    {
                        string[] parts = line.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length > 1) bestMove = parts[1];
                        break;
                    }
                }
                try { process.StandardInput.WriteLine("quit"); } catch { }
                try { process.WaitForExit(2000); } catch { }
                if (String.IsNullOrEmpty(bestMove))
                    throw new InvalidOperationException("Search ended without a bestmove. Try a lower depth or check the engine build.");
                return new Dictionary<string, object> {
                    { "ok", true }, { "bestmove", bestMove }, { "info", lastInfo },
                    { "depth", depth }, { "nnue", useNnue }
                };
            }
        }
    }

    private static object Handle(string json)
    {
        Json.MaxJsonLength = 1024 * 1024;
        Dictionary<string, object> request = Json.Deserialize<Dictionary<string, object>>(json);
        if (request == null || !request.ContainsKey("type"))
            throw new ArgumentException("Missing request type.");
        string type = Convert.ToString(request["type"]);
        if (type == "ping")
            return new Dictionary<string, object> { { "ok", true }, { "message", "NextGen native host connected." } };
        if (type == "analyse") return Analyse(request);
        throw new ArgumentException("Unsupported request type.");
    }

    public static void Main()
    {
        Stream input = Console.OpenStandardInput();
        Stream output = Console.OpenStandardOutput();
        byte[] header = new byte[4];
        while (ReadExact(input, header, 4))
        {
            int length = BitConverter.ToInt32(header, 0);
            if (length <= 0 || length > 1024 * 1024) break;
            byte[] payload = new byte[length];
            if (!ReadExact(input, payload, length)) break;
            object response;
            try
            {
                response = Handle(Encoding.UTF8.GetString(payload));
            }
            catch (Exception ex)
            {
                response = new Dictionary<string, object> { { "ok", false }, { "error", ex.Message } };
            }
            Send(output, response);
        }
    }
}
