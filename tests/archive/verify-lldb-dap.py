# 测试项目：verify-lldb-dap.py
# 读取 .vscode 的 DAP 配置启动 controlled workflow step 场景, 验证 main/业务断点/局部变量/单步前进/退出 0.
# 入口: python tests/archive/verify-lldb-dap.py; 需要 Windows, 指定 LLDB/Python 环境和已构建示例调试产物.
# 预期: 断点 verified, 停在正确源文件, next 改变行号; 报告写 build/lldb-dap-validation.json.
# 边界: 会启动调试器和被调试进程, 属历史工具链集成探针, 不是 Pause 压力测试.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
import json, os, pathlib, queue, subprocess, threading, time

root = pathlib.Path(__file__).resolve().parents[2]
settings = json.loads((root / '.vscode/settings.json').read_text())
adapter = settings['lldb-dap.executable-path']
launch = json.loads((root / '.vscode/launch.json').read_text())['configurations'][0]
arguments = {key: value for key, value in launch.items() if key not in ('name', 'type', 'request', 'preLaunchTask')}
arguments['program'] = arguments['program'].replace('${workspaceFolder}', str(root))
arguments['cwd'] = str(root)
arguments['args'] = ['step']
env = os.environ.copy()
env.update(settings.get('lldb-dap.environment', {}))
env['PYTHONDONTWRITEBYTECODE'] = '1'
process = subprocess.Popen([adapter], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           cwd=root, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
messages = queue.Queue()
pending = []
errors = []
sequence = 0

def read_messages():
    try:
        while True:
            header = process.stdout.readline()
            if not header:
                raise EOFError('adapter stdout closed')
            if not header.startswith(b'Content-Length:'):
                raise RuntimeError(repr(header))
            length = int(header.split(b':', 1)[1])
            while process.stdout.readline().strip():
                pass
            payload = process.stdout.read(length)
            messages.put(json.loads(payload))
    except Exception as error:
        messages.put(error)

def read_errors():
    errors.append(process.stderr.read().decode('utf-8', 'replace'))

threading.Thread(target=read_messages, daemon=True).start()
error_thread = threading.Thread(target=read_errors, daemon=True)
error_thread.start()

def send(command, arguments):
    global sequence
    sequence += 1
    request = {'seq': sequence, 'type': 'request', 'command': command, 'arguments': arguments}
    payload = json.dumps(request).encode()
    process.stdin.write(f'Content-Length: {len(payload)}\r\n\r\n'.encode() + payload)
    process.stdin.flush()
    return sequence

def wait_for(predicate, timeout=30):
    for index, message in enumerate(pending):
        if predicate(message):
            return pending.pop(index)
    deadline = time.monotonic() + timeout
    while True:
        message = messages.get(timeout=max(0.01, deadline - time.monotonic()))
        if isinstance(message, Exception):
            raise message
        if predicate(message):
            return message
        pending.append(message)

def response(seq):
    message = wait_for(lambda m: m.get('type') == 'response' and m.get('request_seq') == seq)
    if not message.get('success'):
        raise RuntimeError(json.dumps(message))
    return message.get('body', {})

def request(command, arguments):
    return response(send(command, arguments))

def event(name):
    return wait_for(lambda m: m.get('event') == name).get('body', {})

def stack(stopped):
    return request('stackTrace', {'threadId': stopped['threadId'], 'levels': 5})['stackFrames']

report = {}
try:
    report['initialize'] = request('initialize', {'adapterID': 'lldb-dap', 'clientID': 'vscode', 'clientName': 'Visual Studio Code', 'locale': 'en-us', 'supportsProgressReporting': True, 'supportsMemoryReferences': True, 'supportsInvalidatedEvent': True,
        'pathFormat': 'path', 'linesStartAt1': True, 'columnsStartAt1': True,
        'supportsRunInTerminalRequest': False, 'supportsVariableType': True})
    launch_seq = send('launch', arguments)
    event('initialized')
    source = root / 'examples/controlled_business.hpp'
    line = next(i for i, value in enumerate(source.read_text(encoding='utf-8').splitlines(), 1)
                if 'work: begin controlled workflow' in value)
    report['breakpoints'] = request('setBreakpoints', {'source': {'path': str(source)},
        'breakpoints': [{'line': line}]})['breakpoints']
    if not all(item.get('verified') for item in report['breakpoints']):
        raise RuntimeError('business breakpoint was not resolved')
    request('configurationDone', {})
    response(launch_seq)
    main_stop = event('stopped')
    report['main'] = stack(main_stop)
    if 'main' not in report['main'][0]['name']:
        raise RuntimeError('did not stop at main')
    request('continue', {'threadId': main_stop['threadId']})
    work_stop = event('stopped')
    report['work'] = stack(work_stop)
    if pathlib.Path(report['work'][0].get('source', {}).get('path', '')).name != source.name:
        raise RuntimeError('did not stop in business coroutine source')
    report['scopes'] = request('scopes', {'frameId': report['work'][0]['id']})['scopes']
    report['variables'] = []
    for scope in report['scopes']:
        if scope.get('presentationHint') == 'locals' or scope['name'] == 'Locals':
            report['variables'] = request('variables', {'variablesReference': scope['variablesReference']})['variables']
    request('next', {'threadId': work_stop['threadId'], 'granularity': 'line'})
    step_stop = event('stopped')
    report['step'] = stack(step_stop)
    if report['step'][0]['line'] == report['work'][0]['line']:
        raise RuntimeError('next did not advance the source line')
    request('setBreakpoints', {'source': {'path': str(source)}, 'breakpoints': []})
    request('continue', {'threadId': step_stop['threadId']})
    report['exited'] = event('exited')
    if report['exited']['exitCode'] != 0:
        raise RuntimeError('example failed under LLDB')
    event('terminated')
    request('disconnect', {'terminateDebuggee': True})
    report['passed'] = True
    print(json.dumps({key: report[key] for key in ['breakpoints', 'main', 'work', 'step', 'variables', 'exited', 'passed']}, indent=2))
finally:
    if process.poll() is None:
        try:
            request('disconnect', {'terminateDebuggee': True})
        except Exception:
            pass
        if process.poll() is None:
            process.kill()
    process.wait(timeout=10)
    error_thread.join(timeout=2)
    report['adapter_exit'] = process.returncode
    report['stderr'] = ''.join(errors)
    (root / 'build/lldb-dap-validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')