"""
STM32 OTA 上位机 —— 逐包 CRC32 校验 + 停等重传

=====================================================================
 怎么用(在 Windows 的 PowerShell 里执行):
    py ota_upload.py --list            列出电脑上所有串口
    py ota_upload.py --selftest        不连设备,自检 CRC32 是否正确
    py ota_upload.py COM5 app.bin      真正升级:把 app.bin 发给开发板

 参数说明:
    port          串口号,例如 COM5(用 --list 查看)
    firmware      要升级的固件 .bin 文件(注意:是"应用程序"的 bin)
    -b/--baud     波特率,默认 230400
    --payload     每包数据长度,默认 512(必须和单片机端一致)
    --retry       每包最多重传几次,默认 5
    --wait        等你按复位键的秒数,默认 30
=====================================================================
"""

import argparse
import sys
import time
import zlib

# ---------- 协议常量(必须和单片机端完全一致,不要随便改)----------
SYNC       = 0xA5                      # 数据帧同步头
CMD_HDR    = bytes([0xA5, 0x5A, 0x01]) # 命令帧帧头
CMD_LEN    = 20                        # 命令帧总长度

CTRL_READY = 0x11   # 单片机 -> 电脑:擦除完成,可以开始发包
CTRL_ACK   = 0x06   # 单片机 -> 电脑:本包接收成功
CTRL_NAK   = 0x15   # 单片机 -> 电脑:本包校验失败,请重发
CTRL_DONE  = 0x12   # 单片机 -> 电脑:全部包已收到,开始最终校验
CTRL_OK    = 0x13   # 单片机 -> 电脑:最终 CRC 校验通过
CTRL_FAIL  = 0x14   # 单片机 -> 电脑:出错了(命令被拒 / 写入越界 / 最终校验失败)

CTRL_ALL   = {CTRL_READY, CTRL_ACK, CTRL_NAK, CTRL_DONE, CTRL_OK, CTRL_FAIL}
# 单片机启动后会先打印一段文字(固件里是 GBK 中文), 收到任意数据就算它启动了


# =====================================================================
#  1. CRC32(标准 IEEE 802.3 / zlib)
#     自检向量: crc32(b"123456789") 必须等于 0xCBF43926
# =====================================================================
def crc32(data: bytes) -> int:
    """直接调用 Python 自带的 zlib,和单片机端的软件 CRC32 结果一致。"""
    return zlib.crc32(data) & 0xFFFFFFFF


def crc32_manual(data: bytes) -> int:
    """手写版本(和 C 语言端逐位算法一模一样),仅用于自检,证明两端一致。"""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF


# =====================================================================
#  2. 构造命令帧(20 字节)
#     0~2    A5 5A 01         帧头
#     3~6    固件总长度        大端
#     7~8    总包数            大端
#     9~10   每包长度          大端
#     11     协议版本 0x01
#     12~15  全文件 CRC32      大端
#     16~19  本命令帧 CRC32    小端(对 0~15 计算)
# =====================================================================
def build_cmd(app_len, total, payload_len, whole_crc):
    f = bytearray(CMD_HDR)
    f += app_len.to_bytes(4, "big")
    f += total.to_bytes(2, "big")
    f += payload_len.to_bytes(2, "big")
    f += bytes([0x01])
    f += whole_crc.to_bytes(4, "big")
    f += crc32(bytes(f)).to_bytes(4, "little")
    assert len(f) == CMD_LEN, "命令帧长度错误: %d" % len(f)
    return bytes(f)


# =====================================================================
#  3. 构造数据帧
#     0      A5               同步头
#     1~2    seq              小端,从 1 开始
#     3~4    本包数据长度      小端
#     5~     数据
#     末尾4  CRC32             小端(覆盖同步头到数据)
# =====================================================================
def build_frame(seq, payload):
    f = bytearray([SYNC])
    f += seq.to_bytes(2, "little")
    f += len(payload).to_bytes(2, "little")
    f += payload
    f += crc32(bytes(f)).to_bytes(4, "little")
    return bytes(f)


# =====================================================================
#  4. 串口读取(带缓冲区,绝不丢字节)
# =====================================================================
_rx = bytearray()          # 已从串口读出、但还没处理的字节

def log_text(data: bytes):
    """把单片机打印的文字显示出来(调试信息)"""
    s = data.decode("gbk", "ignore").strip()
    if s:
        print("  [设备] " + s.replace("\r", "").replace("\n", "\n  [设备] "))


def next_ctrl(ser, timeout, want=None):
    """等待单片机回的控制字节。
       - 非控制字节(printf 文字)会被打印出来;
       - 已经读进来但这次用不到的字节留在缓冲区,下次继续用,不会丢。"""
    end = time.time() + timeout
    text = bytearray()
    while True:
        while _rx:
            b = _rx.pop(0)
            if b in CTRL_ALL:
                if text:
                    log_text(bytes(text))
                    text.clear()
                if want is None or b in want:
                    return b
                # 是控制字节但不是这次要等的,丢弃后继续
            else:
                text.append(b)
        if time.time() >= end:
            if text:
                log_text(bytes(text))
            return None
        chunk = ser.read(64)
        if chunk:
            _rx.extend(chunk)


# =====================================================================
#  5. 三种模式:--list / --selftest / 真正升级
# =====================================================================
def mode_list():
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    if not ports:
        print("没有找到任何串口。请插好 USB-TTL,并确认驱动已安装。")
        return 1
    print("电脑上可用的串口:")
    for p in ports:
        print("   %-8s  %s" % (p.device, p.description))
    print("\n把上面 device 那一列的名字(如 COM5)填到命令行里。")
    return 0


def mode_selftest(payload_len):
    print("== 自检(不连接设备)==")
    vec = b"123456789"
    a = crc32(vec)
    b = crc32_manual(vec)
    ok = (a == 0xCBF43926) and (b == 0xCBF43926)

    print("zlib 算法  CRC32(\"123456789\") = 0x%08X" % a)
    print("手写算法  CRC32(\"123456789\") = 0x%08X" % b)
    print("期望值                          = 0xCBF43926")
    print("结论: %s" % ("正确,两种算法一致" if ok else "错误!"))

    c = build_cmd(1024, 2, payload_len, 0x12345678)
    print("命令帧(%d 字节):" % len(c), " ".join("%02X" % x for x in c))
    fr = build_frame(1, bytes(range(8)))
    print("示例数据帧(%d 字节):" % len(fr), " ".join("%02X" % x for x in fr))
    print("== 自检结束 ==")
    return 0 if ok else 1


def mode_upload(args):
    import serial
    from serial.tools import list_ports

    # 读固件
    if not (args.firmware.lower().endswith(".bin")):
        print("提示:固件一般是 .bin 文件,请确认你给的是二进制而不是 .hex。")
    try:
        with open(args.firmware, "rb") as fp:
            fw = fp.read()
    except OSError as e:
        print("打不开固件文件: %s" % e)
        return 1
    if len(fw) == 0:
        print("固件文件是空的,退出。")
        return 1

    total = (len(fw) + args.payload - 1) // args.payload
    whole = crc32(fw)
    print("固件: %s" % args.firmware)
    print("大小: %d 字节, 分包: %d 包 (每包 %d 字节)" % (len(fw), total, args.payload))
    print("全文件 CRC32 = 0x%08X" % whole)
    print("-" * 50)

    # 打开串口
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=2)
    except Exception as e:
        print("打开串口 %s 失败: %s" % (args.port, e))
        print("常见原因:串口号写错 / 串口被别的软件(如串口助手)占用。")
        print("可用的串口:")
        for p in list_ports.comports():
            print("   %-8s  %s" % (p.device, p.description))
        return 1

    try:
        _rx.clear()
        ser.reset_input_buffer()
        print("串口已打开: %s @ %d" % (args.port, args.baud))
        print("请按一下开发板上的【复位】键(或重新上电),脚本在等你...")

        # 等开机打印: 收到任意数据就认为板子已经启动并进入接收窗口
        deadline = time.time() + args.wait
        got_banner = False
        while time.time() < deadline and not got_banner:
            chunk = ser.read(64)
            if chunk:
                log_text(chunk)
                got_banner = True
                time.sleep(0.2)     # 等它把启动打印发完, 并执行到 HAL_UART_Receive_IT
        if not got_banner:
            print("没有等到开发板的任何数据。")
            print("检查: 1) 串口号 2) 波特率(本工程 230400) 3) 有没有按复位键")
            return 1

        # 发命令帧
        print("已收到启动提示,发送升级命令...")
        ser.write(build_cmd(len(fw), total, args.payload, whole))
        ser.flush()

        print("等待擦除完成(可能几秒钟)...")
        r = next_ctrl(ser, 20.0, {CTRL_READY, CTRL_FAIL})
        if r == CTRL_FAIL:
            print("设备拒绝了升级请求(固件大小超出分区,或命令帧校验失败)。")
            return 1
        if r != CTRL_READY:
            print("没有收到 READY,擦除可能失败了。")
            return 1
        print("板子就绪,开始传固件...")

        # 逐包发送
        seq = 1
        sent = 0
        t0 = time.time()
        while seq <= total:
            payload = fw[(seq - 1) * args.payload: seq * args.payload]
            frame = build_frame(seq, payload)
            ok = False
            for attempt in range(1, args.retry + 1):
                ser.write(frame)
                ser.flush()
                sent += 1
                r = next_ctrl(ser, args.ack_timeout, {CTRL_ACK, CTRL_NAK, CTRL_FAIL})
                if r == CTRL_ACK:
                    ok = True
                    break
                if r == CTRL_FAIL:
                    print("\n设备报错并中止(疑似超出分区),第 %d 包停住。" % seq)
                    return 1
                why = "NAK" if r == CTRL_NAK else "超时"
                print("  包 %d %s,重发第 %d 次" % (seq, why, attempt))
            if not ok:
                print("包 %d 重传 %d 次仍失败,中止。" % (seq, args.retry))
                return 1
            seq += 1
            pct = (seq - 1) * 100 // total
            print("\r进度: %d/%d 包 (%d%%)" % (seq - 1, total, pct), end="", flush=True)
        print()

        # 等最终结果
        print("发送完毕,等待单片机最终校验...")
        r = next_ctrl(ser, 30.0, {CTRL_OK, CTRL_FAIL})
        dt = time.time() - t0
        if r == CTRL_OK:
            print("升级成功!共 %d 字节,耗时 %.1f 秒,发送 %d 帧。" % (len(fw), dt, sent))
            return 0
        print("最终校验失败(CTRL_FAIL),请重试。")
        return 1
    except KeyboardInterrupt:
        print("\n已被你手动中断。")
        return 130
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(
        description="STM32 OTA 上位机(逐包 CRC32 + 停等重传)",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="串口号,如 COM5")
    ap.add_argument("firmware", nargs="?", help="要升级的固件 .bin 文件")
    ap.add_argument("-b", "--baud", type=int, default=230400, help="波特率,默认 230400")
    ap.add_argument("--payload", type=int, default=512, help="每包数据长度,默认 512")
    ap.add_argument("--retry", type=int, default=5, help="每包最多重传次数,默认 5")
    ap.add_argument("--ack-timeout", type=float, default=2.0, help="等 ACK 的超时秒数")
    ap.add_argument("--wait", type=float, default=30.0, help="等复位键的秒数,默认 30")
    ap.add_argument("--list", action="store_true", help="列出所有串口")
    ap.add_argument("--selftest", action="store_true", help="不连设备,自检 CRC32")
    args = ap.parse_args()

    if args.list:
        try:
            import serial  # noqa: F401
        except ImportError:
            print("还没安装 pyserial,请先执行:  py -m pip install pyserial")
            return 1
        return mode_list()

    if args.selftest:
        return mode_selftest(args.payload)

    if not args.port or not args.firmware:
        ap.print_help()
        print("\n例子:  py ota_upload.py COM5 app.bin")
        return 1

    try:
        import serial  # noqa: F401
    except ImportError:
        print("还没安装 pyserial,请先执行:  py -m pip install pyserial")
        return 1

    if args.payload != 512:
        print("提醒:你用了 --payload %d,单片机端也要是同样的值,否则会失败。"
              % args.payload)
    return mode_upload(args)


if __name__ == "__main__":
    sys.exit(main())
