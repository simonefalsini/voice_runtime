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
        
        # Temporarily run git add -N to track untracked files for diffing
        run_cmd(["git", "add", "-N", "."], cwd=item_path)
        
        # Get list of modified files
        modified_files_str = run_cmd(["git", "diff", "--name-only", "--ignore-submodules=all"], cwd=item_path)
        modified_files = modified_files_str.splitlines() if modified_files_str else []
        
        # Also check staged but uncommitted files
        staged_files_str = run_cmd(["git", "diff", "--cached", "--name-only", "--ignore-submodules=all"], cwd=item_path)
        staged_files = staged_files_str.splitlines() if staged_files_str else []
        
        all_changed_files = list(set(modified_files + staged_files))
        
        # Filter out mode-only changes or empty diffs
        actual_changed_files = []
        for file_path in all_changed_files:
            # Skip local user/IDE/build files
            if any(x in file_path for x in [".swiftpm/", ".DS_Store", "xcuserdata/", ".git"]):
                continue
            # Skip specific auto-generated files
            if item == "WebRTC" and file_path == "experiments/registered_field_trials.h":
                continue
            diff_content = run_cmd(["git", "diff", "HEAD", "--ignore-submodules=all", "--", file_path], cwd=item_path)
            if diff_content and diff_content.strip():
                # Check if it has actual additions or deletions
                has_diff = False
                for line in diff_content.splitlines():
                    if (line.startswith('+') or line.startswith('-')) and not (line.startswith('+++') or line.startswith('---')):
                        has_diff = True
                        break
                if has_diff:
                    actual_changed_files.append(file_path)
                    
        # Gather Git metadata
        commit_hash = run_cmd(["git", "rev-parse", "HEAD"], cwd=item_path)
        remote_url = run_cmd(["git", "remote", "get-url", "origin"], cwd=item_path)
        branch = run_cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=item_path)
        
        dep_patch_dir = os.path.join(patches_root, item)
        metadata_path = os.path.join(dep_patch_dir, "metadata.json")
        
        needs_update = False
        existing_meta = None
        if os.path.exists(metadata_path):
            try:
                with open(metadata_path, "r", encoding="utf-8") as f:
                    existing_meta = json.load(f)
                
                # Check if metadata is out of date
                if existing_meta.get("commit") != commit_hash:
                    needs_update = True
                elif existing_meta.get("branch") != branch:
                    needs_update = True
                elif existing_meta.get("repository") != remote_url:
                    needs_update = True
                elif existing_meta.get("modified_files", []) != actual_changed_files:
                    needs_update = True
            except Exception:
                needs_update = True
        else:
            needs_update = True
            
        if needs_update or actual_changed_files:
            print(f"Updating metadata/patches for dependency: {item}...")
            os.makedirs(dep_patch_dir, exist_ok=True)
            
            # Write metadata.json
            metadata = {
                "repository": remote_url,
                "commit": commit_hash,
                "branch": branch,
                "timestamp": datetime.now().isoformat(),
                "modified_files": actual_changed_files
            }
            
            with open(metadata_path, "w", encoding="utf-8") as f:
                json.dump(metadata, f, indent=4)
                
            # Clean up old files inside the patch directory
            for file_in_dir in os.listdir(dep_patch_dir):
                if file_in_dir != "metadata.json":
                    p_to_del = os.path.join(dep_patch_dir, file_in_dir)
                    if os.path.isdir(p_to_del):
                        import shutil
                        shutil.rmtree(p_to_del, ignore_errors=True)
                    else:
                        try:
                            os.remove(p_to_del)
                        except Exception:
                            pass
                            
            if not actual_changed_files:
                print(f"No code modifications in {item}. Metadata updated to commit {commit_hash}.")
            else:
                # Create individual patch files for each modified file
                for file_path in actual_changed_files:
                    diff_res = subprocess.run(["git", "diff", "HEAD", "--ignore-submodules=all", "--", file_path], cwd=item_path, capture_output=True)
                    if diff_res.returncode != 0 or not diff_res.stdout:
                        continue
                    
                    patch_file_dest = os.path.join(dep_patch_dir, file_path + ".patch")
                    os.makedirs(os.path.dirname(patch_file_dest), exist_ok=True)
                    
                    # Write with LF endings
                    with open(patch_file_dest, "wb") as f:
                        f.write(diff_res.stdout.replace(b"\r\n", b"\n"))
                        
                # Create a combined patch file for ease of application
                combined_res = subprocess.run(["git", "diff", "HEAD", "--ignore-submodules=all", "--"] + actual_changed_files, cwd=item_path, capture_output=True)
                if combined_res.returncode == 0 and combined_res.stdout:
                    combined_patch_path = os.path.join(combined_patch_path if 'combined_patch_path' in locals() else os.path.join(dep_patch_dir, "combined.patch"), "wb")
                    # Wait, let's use the explicit target path:
                    combined_patch_path = os.path.join(dep_patch_dir, "combined.patch")
                    with open(combined_patch_path, "wb") as f:
                        f.write(combined_res.stdout.replace(b"\r\n", b"\n"))
                        
                print(f"Successfully generated patches for {item} in patches/{item}/")
        else:
            print(f"No metadata/patch update needed for {item} (already at commit {commit_hash} without modifications).")
            
        # Reset the intent-to-add files so we leave the repo in its original state
        run_cmd(["git", "reset", "."], cwd=item_path)


if __name__ == "__main__":
    main()
