// BedJet Matter Bridge — Web Flasher
//
// The Pages deploy mirrors every release's firmware into firmware/<tag>/ and
// writes firmware/index.json, so everything here is fetched same-origin.
// GitHub release assets themselves are NOT CORS-fetchable from a browser.

// chip key (from firmware_<chip>_combined.bin) -> esp-web-tools chipFamily.
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

async function fetchFirmwareIndex() {
    try {
        const res = await fetch("firmware/index.json", { cache: "no-cache" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return await res.json();
    } catch (error) {
        console.error("Failed to load firmware/index.json:", error);
        return null;
    }
}

function toHex(buffer) {
    return [...new Uint8Array(buffer)].map(b => b.toString(16).padStart(2, "0")).join("");
}

async function initInstallButton() {
    const versionBadge = document.getElementById("versionBadge");
    const versionSelect = document.getElementById("versionSelect");
    const installButton = document.getElementById("installButton");
    const chipSelect = document.getElementById("chipSelect");
    const noFirmware = document.getElementById("noFirmware");
    const verifyStatus = document.getElementById("verifyStatus");

    const index = await fetchFirmwareIndex();
    // Newest first, as emitted by the Pages deploy.
    const versions = (index && Array.isArray(index.versions)) ? index.versions : [];

    if (versions.length === 0) {
        // No firmware staged. Do NOT leave the install button without a
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

    // Download each part once, verify its SHA-256, and keep the exact verified
    // bytes behind blob URLs so esp-web-tools flashes what we checked. The parts
    // are written at their real offsets and never cover the NVS partition.
    async function verifyBuild(build) {
        const verifiedParts = [];
        for (const part of build.parts) {
            const res = await fetch(part.path, { cache: "no-cache" });
            if (!res.ok) {
                throw new Error(`firmware download failed (HTTP ${res.status})`);
            }
            const bytes = await res.arrayBuffer();
            const actual = toHex(await crypto.subtle.digest("SHA-256", bytes));
            const expected = (part.sha256 || "").toLowerCase();
            if (!expected) {
                return { ok: false, reason: "no published checksum in index.json" };
            }
            if (expected !== actual) {
                return { ok: false, reason: `SHA-256 mismatch for ${part.path.split("/").pop()}` };
            }
            const blobUrl = URL.createObjectURL(
                new Blob([bytes], { type: "application/octet-stream" }));
            verifiedBlobUrls.push(blobUrl);
            verifiedParts.push({ path: blobUrl, offset: part.offset });
        }
        build.verifiedParts = verifiedParts;
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
            // Update in place: never erase, so NVS (Matter commissioning + the
            // saved BedJet pairing) is preserved.
            new_install_prompt_erase: false,
            builds: subset.map(b => ({ chipFamily: b.chipFamily, parts: b.verifiedParts })),
        };
        manifestUrl = URL.createObjectURL(
            new Blob([JSON.stringify(manifest)], { type: "application/json" }));
        installButton.setAttribute("manifest", manifestUrl);
        installButton.style.display = "";
        chipSelect.disabled = false;
    }

    function selectVersion(tag) {
        const chosen = versions.find(v => v.tag === tag) || versions[0];
        activeBuilds = (chosen.builds || []).map(b => ({
            chipFamily: CHIP_FAMILIES[b.chip] || b.chip,
            parts: b.parts || [],
        }));
        versionSelect.value = chosen.tag;
        versionBadge.textContent = chosen.tag;
        versionBadge.style.background = "";

        // Offer only the chips this version actually has firmware for.
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
