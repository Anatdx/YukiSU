#!/usr/bin/env python3
"""
YukiSU SuperKey Hash Generator

用法:
    python superkey_gen.py <your_superkey>
    
示例:
    python superkey_gen.py "my_secret_password_123"
    
输出可以直接用于内核编译:
    -DSUPERKEY_HASH=0x...
"""

import sys

def hash_superkey(key: str) -> int:
    """计算超级密码的哈希值，与内核 superkey.h 一致（按无符号字节）"""
    hash_val = 1000000007
    for c in key:
        # 与内核 (u64)(unsigned char)key[i] 一致
        hash_val = (hash_val * 31 + (ord(c) & 0xFF)) & 0xFFFFFFFFFFFFFFFF
    return hash_val

def main():
    if len(sys.argv) < 2:
        print("用法: python superkey_gen.py <your_superkey>")
        print("示例: python superkey_gen.py 'my_secret_password'")
        sys.exit(1)
    
    key = sys.argv[1]
    hash_val = hash_superkey(key)
    
    print("=" * 60)
    print("YukiSU SuperKey 配置")
    print("=" * 60)
    print(f"超级密码: {key}")
    print(f"哈希值:   0x{hash_val:016x}")
    print()
    print("在内核编译时添加以下参数:")
    print(f"  -DCONFIG_KSU_SUPERKEY -DSUPERKEY_HASH=0x{hash_val:016x}ULL")
    print()
    print("或在 Makefile/Kbuild 中添加:")
    print(f"  ccflags-y += -DCONFIG_KSU_SUPERKEY")
    print(f"  ccflags-y += -DSUPERKEY_HASH=0x{hash_val:016x}ULL")
    print("=" * 60)

if __name__ == "__main__":
    main()
