# Deploy EMD Agent to Local K3s Cluster

## Quick Deploy

Since the k3s cluster is on a remote node (`andrew-ubuntu-2404`), we need to import the Docker image:

### Option 1: Manual Import (Recommended)

1. **Save the image locally:**
   ```bash
   docker save emd-agent:latest -o /tmp/emd-agent.tar
   ```

2. **Copy to the k3s node:**
   ```bash
   scp /tmp/emd-agent.tar andrew@andrew-ubuntu-2404:/tmp/
   ```

3. **SSH into the node and import:**
   ```bash
   ssh andrew@andrew-ubuntu-2404
   sudo k3s ctr images import /tmp/emd-agent.tar
   sudo k3s ctr images ls | grep emd-agent
   exit
   ```

4. **Deploy:**
   ```bash
   kubectl apply -f k8s/deployment.yaml
   ```

### Option 2: Use Local Registry

Set up a local registry that k3s can access:

```bash
# On andrew-ubuntu-2404:
docker run -d -p 5000:5000 --name registry registry:2

# Tag and push from dev machine:
docker tag emd-agent:latest andrew-ubuntu-2404:5000/emd-agent:latest
docker push andrew-ubuntu-2404:5000/emd-agent:latest

# Update deployment.yaml to use:
# image: andrew-ubuntu-2404:5000/emd-agent:latest
```

## Current Status

Deployment is created but pod is in `ImagePullBackOff` because the image needs to be imported into k3s.
