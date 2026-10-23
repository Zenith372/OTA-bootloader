"""
STM32 波特率计算器 —— 算任意波特率的分频值、实际波特率和误差

STM32 的波特率不是"选档位",而是"用 BRR 寄存器分频算出来的":
    波特率 = 串口时钟 / (16 x 分频值)      分频值 = 整数 + (小数/16)
所以任何值都能设,关键看"误差大不大"。

用法:
    py stm32_baud.py                    # 默认 72MHz,列出常用档位
    py stm32_baud.py 72 230400          # 72MHz 下算 230400
    py stm32_baud.py 8 115200           # 8MHz 下算 115200
    py stm32_baud.py 36                 # 36MHz 下列出常用档位
    py stm32_baud.py 72 250000 500000   # 一次算多个
"""
import sys

COMMON = [1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 76800,
          115200, 128000, 153600, 230400, 250000, 256000, 460800, 500000,
          921600, 1000000, 1500000, 2000000, 3000000]


def calc(fck, baud, over8=False):
    os_val = 8 if over8 else 16
    div = fck / (os_val * baud)
    mant = int(div)
    frac = round((div - mant) * os_val)
    if frac == os_val:
        mant += 1
        frac = 0
    actual = fck / (os_val * (mant + frac / os_val))
    err = (actual - baud) / baud * 100.0
    brr = (mant << 4) | frac
    return brr, mant, frac, actual, err


def show(fck, baud):
    brr, mant, frac, actual, err = calc(fck, baud)
    if abs(err) < 0.5:
        flag = 'OK(很准)'
    elif abs(err) < 1.0:
        flag = 'OK'
    elif abs(err) < 2.5:
        flag = '可用(余量小)'
    else:
        flag = '误差太大!'
    print('  %-9d  BRR=0x%04X  div=%d+%2d/16  实际=%-9d  误差=%+8.4f%%   %s'
          % (baud, brr, mant, frac, round(actual), err, flag))


def main():
    args = sys.argv[1:]
    fck = 72e6
    if args:
        fck = float(args[0]) * 1e6

    print('串口时钟 fCK = %g MHz' % (fck / 1e6))
    print('可生成范围: 约 %d ~ %d baud   (超出这个范围设不出来)'
          % (round(fck / (16 * 4095.9375)), round(fck / 16)))
    print()

    if len(args) >= 2:
        for v in args[1:]:
            show(fck, int(v))
    else:
        for b in COMMON:
            show(fck, b)
    print()
    print('说明: 误差 < 约 2~3% 就没问题(两端误差之和);< 0.5% 最好。')


if __name__ == '__main__':
    main()
