#!/usr/bin/env python3
"""
为所有 commands.xml 中 <group> 下的 <element> 注射 id="N" 序号属性。
每个 group 内的 element 从 1 开始编号。
"""

import re
import sys
import os

def process_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # 用正则找到每个 <elements>...</elements> 块
    def add_ids_to_elements_block(match):
        block = match.group(0)
        counter = [0]

        def replace_element_tag(m):
            counter[0] += 1
            tag = m.group(0)
            # 如果已经有 id= 属性，先移除
            tag = re.sub(r'\s+id="[^"]*"', '', tag)
            # 在 <element 后面插入 id="N"
            tag = tag.replace('<element ', f'<element id="{counter[0]}" ', 1)
            return tag

        # 匹配 <element ...> 开标签（可能跨行，但通常在一行内）
        block = re.sub(r'<element\s[^>]*>', replace_element_tag, block)
        return block

    new_content = re.sub(
        r'<elements>.*?</elements>',
        add_ids_to_elements_block,
        content,
        flags=re.DOTALL
    )

    if new_content != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"  ✓ Updated: {filepath}")
    else:
        print(f"  - No changes: {filepath}")


def main():
    src_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    src_dir = os.path.normpath(src_dir)

    files = []
    for root, dirs, filenames in os.walk(src_dir):
        for fn in filenames:
            if fn == 'commands.xml':
                files.append(os.path.join(root, fn))

    if not files:
        print("No commands.xml files found!")
        sys.exit(1)

    print(f"Found {len(files)} commands.xml files:\n")
    for f in sorted(files):
        process_file(f)

    print("\nDone!")


if __name__ == '__main__':
    main()
