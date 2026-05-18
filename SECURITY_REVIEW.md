# Security Review - 2026-05-18

## ⚠️ CRITICAL: Credentials Found in Git History

### Summary
Found multiple credentials committed to git history that need immediate action.

---

## 🔴 HIGH PRIORITY - Immediate Actions Required

### 1. GitHub Personal Access Token
**Location**: `TODO.md` (commit `5919189`)  
**Token**: `***REDACTED_GITHUB_TOKEN***`  
**Status**: ✅ Removed from working tree (commit `508afdc`)

**Action Required**:
1. ⚠️ **Revoke this token immediately** at: https://github.com/settings/tokens
2. GitHub may have already auto-detected and revoked it if repo was pushed
3. Generate new token with `write:packages` scope for future deployments

---

### 2. Camera RTSP Credentials
**Password**: `***REDACTED_PASSWORD***` (URL-encoded as `***REDACTED_PASSWORD***`)  
**Locations in git history**:
- `configs/axis_p4708.toml` (multiple commits)
- `README_PHASE2.md` (deleted file, commit `80abad0`)

**Status**: ✅ Config file removed from tracking (commit `508afdc`)

**Action Required**:
1. ⚠️ **Change camera password** if this is a real/production password
2. Update all camera configurations with new credentials
3. Ensure `k8s/agent.toml` (gitignored) uses new password

---

## 🟡 MEDIUM PRIORITY - Git History Cleanup

### Current State
- Credentials are removed from working tree ✅
- Credentials still exist in git history ⚠️
- If repo has been pushed to GitHub: credentials are public ❌

### Options

#### Option 1: If repository is PRIVATE and never been shared
Use `git-filter-repo` to remove sensitive data from history:

```bash
# Install git-filter-repo
brew install git-filter-repo  # macOS
# or
pip install git-filter-repo

# Create backup first
cp -r .git .git.backup

# Remove sensitive content
git filter-repo --replace-text <(cat <<'EOF'
***REDACTED_GITHUB_TOKEN***==>REDACTED_TOKEN
***REDACTED_PASSWORD***==>REDACTED_PASSWORD
***REDACTED_PASSWORD***==>REDACTED_PASSWORD
EOF
)

# Force push to rewrite remote history (WARNING: affects all clones)
git push --force --all origin
```

#### Option 2: If repository is PUBLIC or widely shared
- Accept that credentials are public
- Rotate/revoke all exposed credentials
- Continue with fresh credentials going forward
- Document the incident in security logs

---

## ✅ GOOD: What's Protected

### Files Correctly Gitignored
- `k8s/agent.toml` - Contains 14 camera credentials ✅
- `k8s/camera_list.txt` - Contains camera IPs ✅
- `configs/*.toml` - Now excluded ✅

### Kubernetes Secrets
- Image pull secret created via kubectl (not in git) ✅
- Secret contains GitHub token but only in cluster, not in repo ✅

### Current Deployment
- Running from GHCR image (credentials not in container) ✅
- Config mounted via ConfigMap (credentials in cluster only) ✅

---

## 🔒 Recommendations Going Forward

### 1. Secret Management
- [ ] Use sealed secrets for k8s deployments
- [ ] Store credentials in environment variables or secret managers
- [ ] Never commit `.toml`, `.env`, or config files with credentials
- [ ] Use `.example` files with placeholder credentials for documentation

### 2. Pre-commit Hooks
Add git pre-commit hook to prevent credential leaks:

```bash
#!/bin/bash
# .git/hooks/pre-commit

# Check for common secret patterns
if git diff --cached | grep -E "(ghp_|github_pat_|password.*=.*[^x])"; then
    echo "❌ ERROR: Potential secret detected in commit"
    echo "Review your changes and remove any credentials"
    exit 1
fi
```

### 3. GitHub Security
- Enable secret scanning on GitHub repository
- Enable push protection to block secret commits
- Rotate tokens regularly (90-day maximum)

### 4. Camera Security
- Use unique passwords per camera
- Rotate credentials quarterly
- Use certificate-based auth if cameras support it

---

## 📋 Checklist

- [ ] Revoke GitHub token `***REDACTED_GITHUB_TOKEN***`
- [ ] Change camera password `***REDACTED_PASSWORD***`
- [ ] Update `k8s/agent.toml` with new credentials
- [ ] Update GHCR image pull secret with new token
- [ ] Decide on git history cleanup approach
- [ ] Enable GitHub secret scanning
- [ ] Set up pre-commit hooks
- [ ] Document credential rotation schedule

---

## Status: ⚠️ REQUIRES IMMEDIATE ACTION

The repository is safe for continued development after credential rotation, but exposed credentials must be changed ASAP.
