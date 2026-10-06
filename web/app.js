// BedJet Matter Bridge — Web Flasher
const GITHUB_OWNER = "tfleck";
const GITHUB_REPO = "bedjet-matter-bridge";

// chip key -> the chipFamily name esp-web-tools expects.
const CHIP_FAMILIES = {
    esp32: "ESP32",
    esp32s2: "ESP32-S2",
    esp32s3: "ESP32-S3",
    esp32c3: "ESP32-C3",
    esp32c6: "ESP32-C6",
    esp32h2: "ESP32-H2",
};

function checkBrowserSupport() {
    const hasWebSerial = "serial" in navigator;
    const warning = document.getElementById("browserWarning");
    if (!hasWebSerial) {
        warning.style.display = "block";
    }
}

// Newest first (GitHub returns releases in created_at descending order).
async function fetchReleases() {
    const apiUrl = `https://api.github.com/repos/${GITHUB_OWNER}/${GITHUB_REPO}/releases?per_page=30`;
    try {
        const response = await fetch(apiUrl, {
            headers: { "Accept": "application/vnd.github.v3+json" }
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error("Failed to fetch releases:", error);
        return [];
    }
}

// Match assets by exact name so "esp32" cannot match "esp32c6". Each firmware
// carries its own <name>.sha256 asset; GitHub also attaches an immutable
// `digest` to every asset, used as a fallback verification source.
function buildsFromRelease(release) {
    const builds = [];
    for (const [chipKey, chipFamily] of Object.entries(CHIP_FAMILIES)) {
        const name = `firmware_${chipKey}_combined.bin`;
        const asset = release.assets.find(a => a.name.toLowerCase() === name);
        if (!asset) {
            continue;
        }
        const shaAsset = release.assets.find(a => a.name.toLowerCase() === `${name}.sha256`);
        builds.push({
            chipFamily: chipFamily,
            url: asset.browser_download_url,
            sha256Url: shaAsset ? shaAsset.browser_download_url : null,
            digest: asset.digest || null,
        });
    }
    return builds;
}

function toHex(buffer) {
    return [...new Uint8Array(buffer)].map(b => b.toString(16).padStart(2, "0")).join("");
}

// Expected SHA-256 for a build: prefer the published per-firmware .sha256,
// fall back to GitHub's asset digest.
async function expectedSha256(build) {
    if (build.sha256Url) {
        try {
            const res = await fetch(build.sha256Url);
            if (res.ok) {
                const match = (await res.text()).match(/\b([0-9a-f]{64})\b/i);
                if (match) {
                    return match[1].toLowerCase();
                }
            }
        } catch (error) {
            console.error("Failed to fetch checksum file:", error);
        }
    }
    if (build.digest && build.digest.startsWith("sha256:")) {
        return build.digest.slice("sha256:".length).toLowerCase();
    }
    return null;
}

async function initInstallButton() {
    const versionBadge = document.getElementById("versionBadge");
    const versionSelect = document.getElementById("versionSelect");
    const installButton = document.getElementById("installButton");
    const chipSelect = document.getElementById("chipSelect");
    const noFirmware = document.getElementById("noFirmware");
    const verifyStatus = document.getElementById("verifyStatus");

    // Every published release that carries firmware becomes a selectable
    // version. GitHub returns releases newest-first, so index 0 is latest.
    const versions = [];
    for (const release of await fetchReleases()) {
        if (release.draft) {
            continue;
        }
        const builds = buildsFromRelease(release);
        if (builds.length > 0) {
            versions.push({ tag: release.tag_name, builds: builds });
        }
    }

    if (versions.length === 0) {
        // No firmware reachable. Do NOT leave the install button without a
        // manifest: esp-web-tools would resolve `null` relative to the page and
        // fetch .../null. Hide it and explain instead.
        versionBadge.textContent = "Unavailable";
        versionBadge.style.background = "#f87171";
        installButton.removeAttribute("manifest");
        installButton.style.display = "none";
        versionSelect.disabled = true;
        chipSelect.disabled = true;
        if (noFirmware) {
            noFirmware.style.display = "block";
        }
        return;
    }

    versionSelect.innerHTML = "";
    versions.forEach((v, i) => {
        const opt = document.createElement("option");
        opt.value = v.tag;
        opt.textContent = i === 0 ? `${v.tag} (latest)` : v.tag;
        versionSelect.appendChild(opt);
    });

    let activeBuilds = [];
    let manifestUrl = null;
    let verifiedBlobUrls = [];

    function setStatus(text, kind) {
        if (!verifyStatus) {
            return;
        }
        verifyStatus.textContent = text || "";
        verifyStatus.className = "verify-status" + (kind ? " " + kind : "");
        verifyStatus.style.display = text ? "block" : "none";
    }

    function releaseVerifiedUrls() {
        for (const url of verifiedBlobUrls) {
            URL.revokeObjectURL(url);
        }
        verifiedBlobUrls = [];
    }

    // Download the firmware once, verify its SHA-256, and keep the exact
    // verified bytes behind a blob URL so esp-web-tools flashes what we checked.
    async function verifyBuild(build) {
        const res = await fetch(build.url);
        if (!res.ok) {
            throw new Error(`firmware download failed (HTTP ${res.status})`);
        }
        const bytes = await res.arrayBuffer();
        const actual = toHex(await crypto.subtle.digest("SHA-256", bytes));
        const expected = await expectedSha256(build);
        if (!expected) {
            return { ok: false, reason: "no published checksum found" };
        }
        if (expected !== actual) {
            return { ok: false, reason: `SHA-256 mismatch (expected ${expected})` };
        }
        const blobUrl = URL.createObjectURL(
            new Blob([bytes], { type: "application/octet-stream" }));
        verifiedBlobUrls.push(blobUrl);
        build.parts = [{ path: blobUrl, offset: 0 }];
        return { ok: true };
    }

    async function showBuilds(selectedChip) {
        if (manifestUrl) {
            URL.revokeObjectURL(manifestUrl);
            manifestUrl = null;
        }
        releaseVerifiedUrls();
        installButton.removeAttribute("manifest");
        installButton.style.display = "none";
        chipSelect.disabled = true;

        const subset = selectedChip
            ? activeBuilds.filter(b => b.chipFamily === selectedChip)
            : activeBuilds;
        if (subset.length === 0) {
            setStatus("", "");
            return;
        }

        setStatus("Verifying firmware…", "pending");
        for (const build of subset) {
            let result;
            try {
                result = await verifyBuild(build);
            } catch (error) {
                result = { ok: false, reason: error.message };
            }
            if (!result.ok) {
                setStatus(`Firmware verification failed: ${result.reason}. Not flashing.`, "error");
                return;
            }
        }

        setStatus("✓ Firmware SHA-256 verified", "ok");

        const manifest = {
            name: "BedJet Matter Bridge",
            version: versionSelect.value,
            // This device does not implement Improv serial, so skip the dialog's
            // post-install Improv wait entirely.
            new_install_improv_wait_time: 0,
            builds: subset.map(b => ({ chipFamily: b.chipFamily, parts: b.parts })),
        };
        manifestUrl = URL.createObjectURL(
            new Blob([JSON.stringify(manifest)], { type: "application/json" }));
        installButton.setAttribute("manifest", manifestUrl);
        installButton.style.display = "";
        chipSelect.disabled = false;
    }

    function selectVersion(tag) {
        const chosen = versions.find(v => v.tag === tag) || versions[0];
        activeBuilds = chosen.builds;
        versionSelect.value = chosen.tag;
        versionBadge.textContent = chosen.tag;
        versionBadge.style.background = "";

        // Offer only the chips that this version actually has firmware for.
        chipSelect.innerHTML = '<option value="">Auto-detect (recommended)</option>';
        for (const build of activeBuilds) {
            const opt = document.createElement("option");
            opt.value = build.chipFamily;
            opt.textContent = build.chipFamily;
            chipSelect.appendChild(opt);
        }
        installButton.style.display = "";
        if (noFirmware) {
            noFirmware.style.display = "none";
        }
        showBuilds("");
    }

    versionSelect.addEventListener("change", () => selectVersion(versionSelect.value));
    chipSelect.addEventListener("change", () => showBuilds(chipSelect.value));
    selectVersion(versions[0].tag);
}

let serialPort = null;
let serialReader = null;
let serialKeepReading = false;
let consoleBuffer = "";

const connectBtn = document.getElementById("connectSerialBtn");
const disconnectBtn = document.getElementById("disconnectSerialBtn");
const serialOutput = document.getElementById("serialOutput");
const consoleOutput = document.getElementById("consoleOutput");

connectBtn.addEventListener("click", async () => {
    try {
        serialPort = await navigator.serial.requestPort();
        await serialPort.open({ baudRate: 115200 });
        serialKeepReading = true;
        serialOutput.style.display = "block";
        connectBtn.style.display = "none";
        readSerial();
    } catch (error) {
        console.error("Serial connection failed:", error);
        if (error.name !== "NotFoundError") {
            alert("Failed to connect: " + error.message);
        }
    }
});

disconnectBtn.addEventListener("click", async () => {
    serialKeepReading = false;
    if (serialReader) { await serialReader.cancel(); }
    if (serialPort) { await serialPort.close(); }
    serialOutput.style.display = "none";
    connectBtn.style.display = "inline-flex";
});

async function readSerial() {
    while (serialPort.readable && serialKeepReading) {
        serialReader = serialPort.readable.getReader();
        try {
            while (true) {
                const { value, done } = await serialReader.read();
                if (done) break;
                const text = new TextDecoder().decode(value);
                consoleBuffer += text;
                consoleOutput.textContent += text;
                document.getElementById("serialConsole").scrollTop = document.getElementById("serialConsole").scrollHeight;

                detectPairingInfo(consoleBuffer);

                if (consoleBuffer.length > 50000) {
                    consoleBuffer = consoleBuffer.slice(-25000);
                }
            }
        } catch (error) {
            console.error("Serial read error:", error);
        } finally {
            serialReader.releaseLock();
        }
    }
    serialOutput.style.display = "none";
    connectBtn.style.display = "inline-flex";
}

// The firmware prints "Manual pairing code: <11 digits>" and "QR payload: MT:..."
// as text (it never renders an ASCII QR), so only those are parsed.
function detectPairingInfo(text) {
    const manualMatch = text.match(/Manual pairing code:\s*(\d{11})/i);
    const qrMatch = text.match(/QR payload:\s*(\S+)/i);

    if (manualMatch) {
        document.getElementById("manualCode").textContent = manualMatch[1];
    }
    if (qrMatch) {
        document.getElementById("qrPayload").textContent = qrMatch[1];
        document.getElementById("qrPayloadRow").style.display = "block";
    }
    if (manualMatch || qrMatch) {
        document.getElementById("qrDisplay").style.display = "block";
    }
}

window.addEventListener("DOMContentLoaded", () => {
    checkBrowserSupport();
    initInstallButton();

    window.addEventListener("beforeunload", async () => {
        if (serialPort) {
            serialKeepReading = false;
            if (serialReader) await serialReader.cancel();
            await serialPort.close();
        }
    });
});
