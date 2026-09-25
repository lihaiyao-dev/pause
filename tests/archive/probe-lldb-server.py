# 测试项目：probe-lldb-server.py
# 启动固定 lldb-dap 的本机 TCP 监听, 建连并发送一次 initialize 请求, 最后关闭进程.
# 入口: python tests/archive/probe-lldb-server.py; 需要 Windows 和脚本中的固定 LLVM/Python 路径.
# 预期: 能连接并打印回复; 当前只记录回复文本, 未断言 JSON success 或完整协议帧, 不验证 Pause/断点行为.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
import os, socket, subprocess, time, json
adapter = os.environ.get('LLDB_DAP_EXECUTABLE', 'lldb-dap')
env = os.environ.copy()
# Set PYTHONHOME in the calling environment only if the selected adapter needs it.
with socket.socket() as reservation:
    reservation.bind(('127.0.0.1', 0))
    port = reservation.getsockname()[1]
p = subprocess.Popen([adapter, '--connection', f'listen://127.0.0.1:{port}', '--connection-timeout', '3'],
                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     env=env, creationflags=subprocess.CREATE_NO_WINDOW)
try:
    for i in range(50):
        try:
            connection = socket.create_connection(('127.0.0.1', port), timeout=1)
            break
        except OSError:
            if p.poll() is not None:
                raise RuntimeError(f'adapter exited {p.returncode}')
            time.sleep(.1)
    else:
        raise RuntimeError('server did not start')
    with connection:
        connection.settimeout(10)
        body = json.dumps({'seq':1,'type':'request','command':'initialize','arguments':{'adapterID':'lldb-dap','clientID':'vscode','pathFormat':'path','linesStartAt1':True,'columnsStartAt1':True}}).encode()
        connection.sendall(f'Content-Length: {len(body)}\r\n\r\n'.encode()+body)
        reply = connection.recv(16384)
        print('reply:', reply.decode('utf-8', 'replace'))
finally:
    if p.poll() is None: p.kill()
    out, err = p.communicate(timeout=5)
    print('exit:', p.returncode)
    print('stdout:', out.decode('utf-8','replace'))
    print('stderr:', err.decode('utf-8','replace'))