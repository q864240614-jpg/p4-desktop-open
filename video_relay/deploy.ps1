$ErrorActionPreference = "Stop"
$hostName = $env:P4_DEPLOY_HOST
$user = $env:P4_DEPLOY_USER
$key = $env:P4_DEPLOY_KEY
$remoteDir = $env:P4_DEPLOY_DIR
if (-not $hostName -or -not $user -or -not $key) {
    throw "Set P4_DEPLOY_HOST, P4_DEPLOY_USER, and P4_DEPLOY_KEY (path to an SSH private key)."
}
if (-not $remoteDir) { $remoteDir = "/home/$user/p4-video-relay" }
$here = $PSScriptRoot
$files = @(
    "server.py",
    "upstream.py",
    "tests.py",
    "index.html",
    "p4-video-relay.service",
    "requirements.txt",
    "install.sh",
    "README.md",
    "P4_DEV.md",
    "live_prod.py",
    "check.py"
)
$target = "${user}@${hostName}"
ssh -o StrictHostKeyChecking=accept-new -i $key $target "mkdir -p $remoteDir"
foreach ($name in $files) {
    scp -i $key (Join-Path $here $name) "${target}:${remoteDir}/$name"
}
ssh -i $key $target "chmod +x $remoteDir/install.sh && VIDEO_RELAY_ROOT=$remoteDir bash $remoteDir/install.sh"
