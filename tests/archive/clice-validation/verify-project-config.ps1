<# 测试项目：clice-validation/verify-project-config.ps1
使用当时项目配置启动 debug/release clice LSP, 验证 snapshot/suspend_point 成员补全.
预期: 成员存在, shutdown 后退出, .clice/server.json 哈希不变; 非 readonly 变体依赖当时项目配置.
入口: powershell -File tests/archive/clice-validation/verify-project-config.ps1; 需要固定 clice 扩展/历史 pause:: API.
缓存及报告保留 build/clice-validation; 属编辑器配置探针, 不验证 Pause 运行期.
归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
#>
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Diagnostics;
using System.Threading;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
public sealed class CliceProbe : IDisposable {
  public Process Process;
  private TcpClient connection;
  private Stream transport;
  public readonly BlockingCollection<string> Messages = new BlockingCollection<string>();
  private readonly StringBuilder errors = new StringBuilder();
  public CliceProbe(string executable, string configuration, string root) {
    var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
    int port=((IPEndPoint)listener.LocalEndpoint).Port; listener.Stop();
    var start = new ProcessStartInfo(executable, "serve --mode socket --host 127.0.0.1 --port " + port + " --configuration " + configuration + " --log-level info");
    start.WorkingDirectory=root; start.UseShellExecute=false; start.CreateNoWindow=true;
    start.RedirectStandardInput=true; start.RedirectStandardOutput=true; start.RedirectStandardError=true;
    Process = System.Diagnostics.Process.Start(start);
    new Thread(() => { string line; while((line=Process.StandardError.ReadLine()) != null) lock(errors) errors.AppendLine(line); }) { IsBackground=true }.Start();
    connection = new TcpClient();
    var deadline=DateTime.UtcNow.AddSeconds(10);
    while(true) {
      try { connection.Connect(IPAddress.Loopback,port); break; }
      catch(SocketException) { if(Process.HasExited || DateTime.UtcNow >= deadline) throw; Thread.Sleep(100); }
    }
    transport = connection.GetStream();
    var stdout = transport;
    new Thread(() => {
      try {
        while (true) {
          var header = new StringBuilder(); int b;
          while ((b=stdout.ReadByte()) != -1) {
            header.Append((char)b);
            if (header.Length >= 4 && header.ToString().EndsWith("\r\n\r\n")) break;
          }
          if (b == -1) break;
          int length=0;
          foreach(var line in header.ToString().Split(new[]{"\r\n"}, StringSplitOptions.RemoveEmptyEntries))
            if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase)) length=int.Parse(line.Substring(15).Trim());
          byte[] body=new byte[length]; int offset=0;
          while(offset < length) { int n=stdout.Read(body,offset,length-offset); if(n==0) throw new EndOfStreamException(); offset+=n; }
          Messages.Add(Encoding.UTF8.GetString(body));
        }
      } catch(Exception ex) { lock(errors) errors.AppendLine(ex.ToString()); }
      finally { Messages.CompleteAdding(); }
    }) { IsBackground=true }.Start();
  }
  public void Send(string json) {
    byte[] body=Encoding.UTF8.GetBytes(json);
    byte[] header=Encoding.ASCII.GetBytes("Content-Length: " + body.Length + "\r\n\r\n");
    var stream=transport;
    stream.Write(header,0,header.Length); stream.Write(body,0,body.Length); stream.Flush();
  }
  public string Errors() { lock(errors) return errors.ToString(); }
  public void Dispose() { if(!Process.HasExited) Process.Kill(); Process.WaitForExit(); connection.Close(); Process.Dispose(); }
}
'@

$root='D:\workspace\pause'
$outRoot=Join-Path $root 'build\clice-validation'
$exe = if ($env:CLICE_EXECUTABLE) { $env:CLICE_EXECUTABLE } else { (Get-Command clice -ErrorAction Stop).Source }
$originalServer=Get-FileHash -LiteralPath (Join-Path $root '.clice\server.json')
$script:requestId=0
$script:overallDeadline=[DateTime]::UtcNow.AddSeconds(120)
$script:transcript=[System.Collections.Generic.List[object]]::new()
function Send-Message($message) {
    $script:transcript.Add(@{direction='send';message=$message})
    $script:probe.Send(($message | ConvertTo-Json -Depth 30 -Compress))
}
function Invoke-Lsp($method,$parameters) {
    $script:requestId++
    $id=$script:requestId
    Send-Message @{jsonrpc='2.0';id=$id;method=$method;params=$parameters}
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    if($deadline -gt $script:overallDeadline) { $deadline=$script:overallDeadline }
    while([DateTime]::UtcNow -lt $deadline) {
        $raw=''
        if(!$script:probe.Messages.TryTake([ref]$raw,1000)) { continue }
        $message=$raw | ConvertFrom-Json
        $script:transcript.Add(@{direction='receive';message=$message})
        if($message.id -eq $id -and !$message.method) {
            if($message.error) { throw ($message.error | ConvertTo-Json -Compress) }
            return $message.result
        }
        if($null -ne $message.id -and $message.method) {
            $result=$null
            if($message.method -eq 'workspace/configuration') { $result=@() }
            Send-Message @{jsonrpc='2.0';id=$message.id;result=$result}
        }
    }
    throw "LSP request timed out: $method"
}

$text=@'
#include "pause.hpp"
void clice_probe(pause::PauseSource& source, pause::PauseBranch& branch) {
    auto snapshot = source.snapshot();
    auto sender = branch.suspend_point();
}
'@
$lines=$text -split "`r?`n"
$uri='file:///D:/workspace/pause/tests/archive/root/main.cpp'
$records=@()
foreach($mode in @('debug','release')) {
    $script:transcript.Clear()
    $script:probe=[CliceProbe]::new($exe,$mode,$root)
    try {
        $capabilities=Invoke-Lsp initialize @{
            processId=$null
            rootUri='file:///D:/workspace/pause'
            workspaceFolders=@(@{uri='file:///D:/workspace/pause';name='pause'})
            capabilities=@{textDocument=@{completion=@{completionItem=@{snippetSupport=$true}}}}
            initializationOptions=@{project=@{
                cache_dir=(Join-Path $outRoot ('lsp-project-config-'+$mode+'-cache')).Replace('\','/')
                enable_indexing=$false
                stateful_worker_count=1
                stateless_worker_count=1
                min_stateless_worker_count=1
                max_stateless_worker_count=1
            }}
        }
        Send-Message @{jsonrpc='2.0';method='initialized';params=@{}}
        Send-Message @{jsonrpc='2.0';method='textDocument/didOpen';params=@{textDocument=@{uri=$uri;languageId='cpp';version=1;text=$text}}}
        $symbols=$null
        $record=@{configuration=$mode;symbols=$symbols;completion=@{}}
        foreach($item in @(@{name='source';line=2;needle='source.';expected='snapshot'},@{name='branch';line=3;needle='branch.';expected='suspend_point'})) {
            $character=$lines[$item.line].IndexOf($item.needle)+$item.needle.Length
            $completion=Invoke-Lsp textDocument/completion @{textDocument=@{uri=$uri};position=@{line=$item.line;character=$character};context=@{triggerKind=1}}
            if($completion.PSObject.Properties.Name -contains 'items') { $items=$completion.items } else { $items=$completion }
            $labels=@($items | ForEach-Object {$_.label})
            $record.completion[$item.name]=$labels
            Write-Output ($mode+' '+$item.name+': '+($labels -join ', '))
            if($labels -notcontains $item.expected) { throw ('Missing LSP member: '+$item.expected) }
        }
        $null=Invoke-Lsp shutdown $null
        Send-Message @{jsonrpc='2.0';method='exit'}
        if(!$script:probe.Process.WaitForExit(10000)) { throw 'LSP server did not exit' }
        $record.exit_code=$script:probe.Process.ExitCode
        $record.diagnostics=@($script:transcript | Where-Object {$_.message.method -eq 'textDocument/publishDiagnostics'} | ForEach-Object {$_.message.params})
        $records+=$record
        Write-Output ($mode+' server exit='+$record.exit_code)
    } finally {
        $script:probe.Dispose()
        [System.IO.File]::WriteAllText((Join-Path $outRoot ('lsp-project-config-'+$mode+'-transcript.json')),($script:transcript | ConvertTo-Json -Depth 40),[System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText((Join-Path $outRoot ('lsp-project-config-'+$mode+'.stderr.log')),$script:probe.Errors(),[System.Text.UTF8Encoding]::new($false))
    }
}
$currentServer=Get-FileHash -LiteralPath (Join-Path $root '.clice\server.json')
if($currentServer.Hash -ne $originalServer.Hash) { throw 'User server record changed' }
[System.IO.File]::WriteAllText((Join-Path $outRoot 'lsp-project-config-results.json'),($records | ConvertTo-Json -Depth 20),[System.Text.UTF8Encoding]::new($false))
