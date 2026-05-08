## PrivMX Endpoint Net Driver
Driver module library used as a dependency in the PrivMX Endpoint library. 
It provides the implementation of the interface for networking.

## Releasing
The current release process is managed from the `main` branch. To create a release, create a version tag in the `vX.Y.Z` format on `main`, push the tag to the remote repository, and then create a GitHub Release from the GitHub UI based on that tag. The changelog should also be generated from the GitHub Release UI.

Example Git commands:

```bash
git checkout main
git pull origin main
git tag vX.Y.Z
git push origin vX.Y.Z
```
