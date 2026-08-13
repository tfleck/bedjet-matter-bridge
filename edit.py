#!/usr/bin/env python3
"""
Convert LF line endings to CRLF for Windows compatibility.
Run this after cloning the repository or before committing.
"""

import os

# Directories to skip (binary/cache folders)
SKIP_DIRS = {'__pycache__', '.git', '.idea', 'node_modules', 'venv'}
SKIP_EXTENSIONS = {'.bin', '.jpg', '.png', '.ico', '.zip', '.tar.gz'}

# Extensions to process (text/code files)
TEXT_EXTENSIONS = {
    '.py', '.js', '.html', '.css', '.json', '.md', '.txt',
    '.yml', '.yaml', '.xml', '.sh', '.bash', '.cmd', '.bat',
    '.cpp', '.c', '.h', '.hpp', '.hpp', '.ino'
}

def convert_line_endings(filepath):
    """Convert LF to CRLF in a single file."""
    try:
        with open(filepath, 'rb') as f:
            content = f.read()
        
        # Check if already has CRLF (no change needed)
        if b'\r\n' in content and b'\n' not in content.replace(b'\r\n', b''):
            return False
        
        # Replace LF with CRLF
        new_content = content.replace(b'\n', b'\r\n')
        
        # Write back if changed
        if new_content != content:
            with open(filepath, 'wb') as f:
                f.write(new_content)
            return True
        return False
        
    except Exception as e:
        print(f"  Error processing {filepath}: {e}")
        return False

def main():
    print("=" * 50)
    print("  Line Ending Converter (LF -> CRLF)")
    print("=" * 50)
    
    # Get target directory
    import sys
    if len(sys.argv) > 1:
        target_dir = sys.argv[1]
    else:
        target_dir = '.'
    
    target_dir = os.path.abspath(target_dir)
    print(f"\nTarget directory: {target_dir}\n")
    
    converted_count = 0
    skipped_count = 0
    error_files = []
    
    for root, dirs, files in os.walk(target_dir):
        # Skip unwanted directories
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        
        for filename in files:
            filepath = os.path.join(root, filename)
            ext = os.path.splitext(filename)[1].lower()
            
            # Skip binary/file types
            if ext in SKIP_EXTENSIONS:
                skipped_count += 1
                continue
            
            # Process text files
            if ext in TEXT_EXTENSIONS or ext == '':
                if convert_line_endings(filepath):
                    print(f"  ✓ {os.path.relpath(filepath, target_dir)}")
                    converted_count += 1
                else:
                    skipped_count += 1
                    
    print(f"\n{'=' * 50}")
    print(f"  Conversion complete!")
    print(f"  Converted: {converted_count} files")
    print(f"  Skipped:   {skipped_count} files")
    print(f"{'=' * 50}")

if __name__ == '__main__':
    main()