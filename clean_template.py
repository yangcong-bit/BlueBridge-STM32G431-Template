import os
import re
import sys
from pathlib import Path

# 【控制台兼容】中文 Windows 的 cmd/PowerShell 默认是 GBK(936)，
# 直接 print emoji 会抛 UnicodeEncodeError 让脚本崩溃。统一把标准输出切到 UTF-8，
# 万一环境不支持也退化为“永不抛异常”，保证这个工具一定能跑完。
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# 设置输入和输出文件夹
# 【重要】以脚本自身所在目录为基准，这样在任何工作目录下双击/执行都指向同一个工程。
BASE_DIR    = Path(__file__).resolve().parent
SOURCE_DIR  = BASE_DIR / 'APP'
OUTPUT_DIR  = BASE_DIR / 'APP_Lite'

# 输出统一采用 Windows 行尾，与工程中 Core/、Drivers/ 保持一致（Keil 工程以 Windows 为主）。
NEWLINE = '\r\n'

# 源文件编码优先级：先按 UTF-8(严格) 试，失败再退化为 GB18030 / GBK。
# GB18030 是 GBK 的超集，能覆盖更全的字节组合，因此排在 GBK 前面更安全。
SOURCE_ENCODINGS = ('utf-8-sig', 'gb18030', 'gbk')

# 出现该字符说明解码时发生了不可逆的字节丢失（文件本身就已经坏了）。
_LOSSY_MARKER = '\ufffd'


def _decode_source(raw_bytes):
    """按候选编码解码源文件，返回 (文本, 编码名, 是否有字节损失)。

    关键点：不使用 errors='ignore'。旧版脚本正是用它静默吞掉无法解码的字节，
    文件被改坏了却没有任何提示。这里的原则是“宁可报警，不可静默”。
    能解出但含替换符时仍然返回文本（交给上层剥离注释后判断），由调用者决定是否报警。
    """
    for enc in SOURCE_ENCODINGS:
        try:
            text = raw_bytes.decode(enc)
        except UnicodeDecodeError:
            continue                                  # 该编码解不开，换下一个
        return text, enc, (_LOSSY_MARKER in text)     # 含替换符 => 有字节损失
    return None, None, True                           # 所有候选编码都失败


def _decode_without_loss(raw_bytes, path):
    """严格版：要求无损解码，失败即抛错。供其它校验脚本调用。"""
    text, enc, lossy = _decode_source(raw_bytes)
    if text is None:
        raise ValueError(
            f"{path.name}: 无法用 {'/'.join(SOURCE_ENCODINGS)} 解码。\n"
            f"    该文件编码混杂或已损坏，请先用编辑器另存为 UTF-8 再重跑本脚本。"
        )
    if lossy:
        raise ValueError(
            f"{path.name}: 文件自身含 {text.count(_LOSSY_MARKER)} 个替换符(U+FFFD)，"
            f"存在不可逆的字节丢失。"
        )
    return text, enc


def remove_comments_and_format(text, filename):
    r"""删除 C 注释、压缩多余空行，并在文件顶部加上 // 文件名。

    采用逐字符状态机而不是正则：
    旧版正则 (".*?"|'.*?')|(/\*.*?\*/|//...) 在 DOTALL 下会让 .*? 跨行配对，
    一旦两个字符串字面量之间夹着注释，就可能把真实代码误判成字符串而保留、
    或把字符串误删，进而“悄悄改坏”代码。状态机严格区分
    字符串 / 字符常量 / 行注释 / 块注释，且绝不跨行误配。
    """
    out    = []        # 已确认要保留的字符
    state  = 'code'    # code | line | block | str | chr
    i, n = 0, len(text)

    while i < n:
        ch  = text[i]
        nxt = text[i + 1] if i + 1 < n else ''

        if state == 'code':
            if ch == '/' and nxt == '*':          # 进入块注释
                state = 'block'; i += 2; continue
            if ch == '/' and nxt == '/':          # 进入行注释
                state = 'line'; i += 2; continue
            if ch == '"':                         # 进入字符串字面量
                state = 'str'
            elif ch == "'":                       # 进入字符常量
                state = 'chr'
            out.append(ch); i += 1; continue

        if state == 'line':
            if ch in '\r\n':                      # 行注释到行尾结束，换行必须保留
                state = 'code'
                out.append(ch); i += 1; continue
            i += 1; continue

        if state == 'block':
            if ch == '*' and nxt == '/':          # 块注释结束
                state = 'code'; i += 2; continue
            if ch in '\r\n':                      # 块注释内部的换行压掉，
                i += 1; continue                  # 避免删注释后代码粘连或在中间断开
            i += 1; continue

        # ---- 字符串 / 字符常量内部：原样搬运，只处理转义 ----
        if ch == '\\' and i + 1 < n:              # 转义序列整体保留（含 \" 和 \'）
            out.append(ch); out.append(nxt); i += 2; continue
        if (state == 'str' and ch == '"') or (state == 'chr' and ch == "'"):
            state = 'code'
        out.append(ch); i += 1

    clean_text = ''.join(out)

    # 1. 压缩连续空行：把 2 个以上空行压成 1 个（\r?\n 兼容 CRLF）
    clean_text = re.sub(r'(?:\r?\n)[ \t]*(?:\r?\n)(?:[ \t]*(?:\r?\n))+', '\n\n', clean_text)
    # 2. 清掉行尾悬空空白（删注释后常留下多余空格）
    clean_text = re.sub(r'[ \t]+(?=\r?\n)', '', clean_text)
    # 3. 行尾统一为 CRLF，与工程 Core/、Drivers/ 保持一致
    clean_text = re.sub(r'\r\n|\r|\n', NEWLINE, clean_text)

    return f"// {filename}{NEWLINE}{clean_text.strip()}{NEWLINE}"

def main():
    src_path = SOURCE_DIR
    out_path = OUTPUT_DIR

    if not src_path.exists() or not src_path.is_dir():
        print(f"[X] 错误：找不到 [{src_path}] 文件夹！")
        print(f"   本脚本必须与 APP 文件夹放在同一目录下。")
        return

    print(f">>> 开始极限压缩模板... 原文件极其安全，精简版将生成在 [{out_path.name}] 中！")
    print(f"   源目录: {src_path}\n")

    # 递归遍历 APP 文件夹下的所有文件
    file_count = 0
    total_before = total_after = 0
    failures = []
    lossy_files = []

    for file in sorted(src_path.rglob('*')):
        if not (file.is_file() and file.suffix.lower() in ['.c', '.h']):
            continue

        relative_path = file.relative_to(src_path)
        target_file = out_path / relative_path

        try:
            # 1. 读取原始字节并解码（不用 errors='ignore'，不静默吞字节）
            raw = file.read_bytes()
            content, used_enc, lossy = _decode_source(raw)

            if content is None:
                raise ValueError(f"无法用 {'/'.join(SOURCE_ENCODINGS)} 解码，编码混杂或文件已损坏")

            # 2. 执行删除注释和格式化
            new_content = remove_comments_and_format(content, file.name)

            # 3. 替换符若在注释里会被第 2 步一并删掉；若精简后仍残留，说明它在真实代码中，
            #    这种情况必须拒绝写盘，否则会把乱码固化进精简版。
            if _LOSSY_MARKER in new_content:
                raise ValueError(f"精简后仍残留 {new_content.count(_LOSSY_MARKER)} 个替换符，"
                                 f"位于真实代码中，已拒绝写盘")

            if lossy:
                lossy_files.append(f"  [WARN]  {relative_path}  (源文件含 {content.count(_LOSSY_MARKER)} 个替换符，"
                                   f"但它们都在注释中，已随注释一并删除)")

            # 4. 写盘：UTF-8 无 BOM
            target_file.parent.mkdir(parents=True, exist_ok=True)
            target_file.write_bytes(new_content.encode('utf-8'))

        except Exception as exc:          # 单个文件失败不中断整体，最后统一汇报
            failures.append(f"  [X] {relative_path}  ->  {exc}")
            continue

        total_before += len(raw)
        total_after  += len(new_content.encode('utf-8'))
        print(f"[OK] 处理完成: {relative_path}   (源编码 {used_enc})")
        file_count += 1

    print(f"\n*** 搞定！共精简了 {file_count} 个文件。")
    if total_before:
        print(f"   体积: {total_before} B -> {total_after} B "
              f"(压缩 {100 - total_after * 100 // total_before}%)")

    if lossy_files:
        print(f"\n[WARN]  以下 {len(lossy_files)} 个源文件在提交时就已损坏（编码转换事故），"
              f"建议修正原文：")
        for item in lossy_files:
            print(item)

    if failures:
        print(f"\n[X] 有 {len(failures)} 个文件未处理：")
        for item in failures:
            print(item)
        raise SystemExit(1)

    print(f"   请去 [{out_path.name}] 文件夹查看你的考场纯净版代码！")

if __name__ == '__main__':
    main()