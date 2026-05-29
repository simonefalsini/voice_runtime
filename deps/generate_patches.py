import os
import subprocess
import json
from datetime import datetime

def run_cmd(cmd, cwd=None):
    res = subprocess.run(cmd, cwd=cwd, capture_output=True)
    if res.returncode != 0:
        return None
    return res.stdout.decode("utf-8", errors="replace").strip()

def main():
    deps_dir = os.path.dirname(os.path.abspath(__file__))
    patches_root = os.path.join(deps_dir, "patches")
    os.makedirs(patches_root, exist_ok=True)
    
    # We scan all subdirectories in deps/
    for item in os.listdir(deps_dir):
        item_path = os.path.join(deps_dir, item)
        if not os.path.isdir(item_path) or item in [".git", "__MACOSX", "patches", "build_onnx_tmp"]:
            continue
            
        git_dir = os.path.join(item_path, ".git")
        if not os.path.exists(git_dir):
            continue
            
        print(f"Checking modifications for dependency: {item}...")
        
        # Get list of modified files
        modified_files_str = run_cmd(["git", "diff", "--name-only"], cwd=item_path)
        modified_files = modified_files_str.splitlines() if modified_files_str else []
        
        # Also check staged but uncommitted files
        staged_files_str = run_cmd(["git", "diff", "--cached", "--name-only"], cwd=item_path)
        staged_files = staged_files_str.splitlines() if staged_files_str else []
        
        all_changed_files = list(set(modified_files + staged_files))
        
        # Filter out mode-only changes or empty diffs
        actual_changed_files = []
        for file_path in all_changed_files:
            diff_content = run_cmd(["git", "diff", "HEAD", "--", file_path], cwd=item_path)
            if diff_content and diff_content.strip():
                # Check if it has actual additions or deletions
                has_diff = False
                for line in diff_content.splitlines():
                    if (line.startswith('+') or line.startswith('-')) and not (line.startswith('+++') or line.startswith('---')):
                        has_diff = True
                        break
                if has_diff:
                    actual_changed_files.append(file_path)
                    
        if not actual_changed_files:
            print(f"No code modifications detected in {item}.")
            continue
            
        print(f"Detected {len(actual_changed_files)} modified files in {item}.")
        
        # Gather Git metadata
        commit_hash = run_cmd(["git", "rev-parse", "HEAD"], cwd=item_path)
        remote_url = run_cmd(["git", "remote", "get-url", "origin"], cwd=item_path)
        branch = run_cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=item_path)
        
        dep_patch_dir = os.path.join(patches_root, item)
        os.makedirs(dep_patch_dir, exist_ok=True)
        
        # Write metadata.json
        metadata = {
            "repository": remote_url,
            "commit": commit_hash,
            "branch": branch,
            "timestamp": datetime.now().isoformat(),
            "modified_files": actual_changed_files
        }
        
        with open(os.path.join(dep_patch_dir, "metadata.json"), "w", encoding="utf-8") as f:
            json.dump(metadata, f, indent=4)
            
        # Create individual patch files for each modified file
        for file_path in actual_changed_files:
            diff_content = run_cmd(["git", "diff", "HEAD", "--", file_path], cwd=item_path)
            if not diff_content:
                continue
            
            patch_file_dest = os.path.join(dep_patch_dir, file_path + ".patch")
            os.makedirs(os.path.dirname(patch_file_dest), exist_ok=True)
            
            # Write with LF endings
            with open(patch_file_dest, "wb") as f:
                f.write(diff_content.encode("utf-8").replace(b"\r\n", b"\n"))
                
        # Create a combined patch file for ease of application
        combined_diff = run_cmd(["git", "diff", "HEAD"], cwd=item_path)
        if combined_diff:
            combined_patch_path = os.path.join(dep_patch_dir, "combined.patch")
            with open(combined_patch_path, "wb") as f:
                f.write(combined_diff.encode("utf-8").replace(b"\r\n", b"\n"))
                
        print(f"Successfully generated patches for {item} in patches/{item}/")

if __name__ == "__main__":
    main()
