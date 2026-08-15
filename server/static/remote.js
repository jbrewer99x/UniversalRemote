let currentDevice = null;
let selectionGeneration = 0;
let statusRefreshInFlight = false;

const deviceSelect = document.getElementById("deviceSelect");
const inputCard = document.getElementById("input-card");
const inputTabs = document.getElementById("inputTabs");
const onlineStatus = document.getElementById("onlineStatus");
const powerStatus = document.getElementById("powerStatus");
const inputStatus = document.getElementById("inputStatus");
const volumeStatus = document.getElementById("volumeStatus");
const connectButton = document.getElementById("connectButton");
const findRemoteButton = document.getElementById("findRemoteButton");
const message = document.getElementById("message");
const headerStatus = document.getElementById("status");

function clearStatusClasses(element) {
    if (!element) return;
    element.classList.remove("status-green", "status-yellow", "status-red");
}
function setMessage(value) { if (message) message.textContent = value; }
function hideInputs() {
    inputTabs.innerHTML = "";
    inputCard.hidden = true;
}
function showInputs() { inputCard.hidden = false; }

async function api(url, options = {}) {
    const response = await fetch(url, {
        cache: "no-store",
        headers: {"Content-Type": "application/json"},
        ...options
    });
    if (!response.ok) {
        const body = await response.json().catch(() => ({}));
        throw new Error(body.detail || `HTTP ${response.status}`);
    }
    return response.json();
}

function renderStatus(status) {
    clearStatusClasses(onlineStatus);
    if (status.paired === false) {
        onlineStatus.textContent = "Not paired";
        onlineStatus.classList.add("status-yellow");
    } else if (status.online) {
        onlineStatus.textContent = "Online";
        onlineStatus.classList.add("status-green");
    } else {
        onlineStatus.textContent = "Offline";
        onlineStatus.classList.add("status-red");
    }

    clearStatusClasses(powerStatus);
    if (status.power === true || status.power === "on") {
        powerStatus.textContent = "On";
        powerStatus.classList.add("status-green");
    } else if (status.power === false || status.power === "off") {
        powerStatus.textContent = "Off";
        powerStatus.classList.add("status-red");
    } else powerStatus.textContent = "Unknown";

    const input = status.input_label ||
        (status.input && typeof status.input === "object" ? status.input.id || status.input.name : status.input) || "--";
    inputStatus.textContent = currentDevice === "pc" ? "--" : input;

    const volume = status.volume && typeof status.volume === "object"
        ? status.volume.display ?? status.volume.raw : status.volume;
    volumeStatus.textContent = volume !== undefined && volume !== null
        ? `${volume}${status.muted || status.mute === true ? " (Muted)" : ""}` : "--";

    if (status.error) setMessage(`Error: ${status.error}`);
}

async function loadDevices() {
    const devices = await api("/api/devices");
    deviceSelect.innerHTML = "";
    devices.forEach(device => {
        const option = document.createElement("option");
        option.value = device.id;
        option.textContent = device.name;
        deviceSelect.appendChild(option);
    });
    if (devices.length) await selectDevice(devices[0].id);
}

async function selectDevice(deviceId) {
    currentDevice = deviceId;
    deviceSelect.value = deviceId;
    const myGeneration = ++selectionGeneration;

    hideInputs();
    inputStatus.textContent = "--";
    volumeStatus.textContent = "--";
    onlineStatus.textContent = "Checking...";
    powerStatus.textContent = "Unknown";

    if (deviceId === "pc") {
        await refreshSelectedStatus(myGeneration);
        return;
    }
    await Promise.allSettled([
        refreshSelectedStatus(myGeneration),
        loadSelectedInputs(myGeneration)
    ]);
}

async function refreshSelectedStatus(generation = selectionGeneration) {
    if (!currentDevice || statusRefreshInFlight) return;
    const selected = currentDevice;
    statusRefreshInFlight = true;
    try {
        const status = await api(`/api/devices/${encodeURIComponent(selected)}/status`);
        if (generation !== selectionGeneration || selected !== currentDevice) return;
        renderStatus(status);
        headerStatus.textContent = `Updated ${new Date().toLocaleTimeString()}`;
    } catch (error) {
        if (generation !== selectionGeneration || selected !== currentDevice) return;
        clearStatusClasses(onlineStatus);
        onlineStatus.textContent = "Offline";
        onlineStatus.classList.add("status-red");
        headerStatus.textContent = `Connection error: ${error.message}`;
    } finally {
        statusRefreshInFlight = false;
    }
}

async function loadSelectedInputs(generation = selectionGeneration) {
    if (!currentDevice || currentDevice === "pc") {
        hideInputs();
        inputStatus.textContent = "--";
        return;
    }

    const selected = currentDevice;
    try {
        const data = await api(`/api/devices/${encodeURIComponent(selected)}/inputs`);
        if (generation !== selectionGeneration || selected !== currentDevice) return;

        inputTabs.innerHTML = "";
        if (!Array.isArray(data.inputs) || data.inputs.length === 0) {
            hideInputs();
            return;
        }

        showInputs();
        data.inputs.forEach(input => {
            const button = document.createElement("button");
            button.className = "inputButton";
            const inputId = typeof input === "string" ? input : input.id;
            const inputLabel = typeof input === "string" ? input : input.label || input.name || input.id;
            button.textContent = inputLabel;

            const currentId = data.current && typeof data.current === "object" ? data.current.id : data.current;
            if (inputId === currentId) button.classList.add("active");

            button.addEventListener("click", async () => {
                const targetDevice = currentDevice;
                const targetGeneration = selectionGeneration;
                try {
                    setMessage(`Switching to ${inputLabel}...`);
                    await api(`/api/devices/${encodeURIComponent(targetDevice)}/inputs/${encodeURIComponent(inputId)}`, {method:"POST"});
                    if (targetDevice !== currentDevice || targetGeneration !== selectionGeneration) return;
                    setMessage(`Input changed to ${inputLabel}`);
                    await Promise.allSettled([
                        refreshSelectedStatus(targetGeneration),
                        loadSelectedInputs(targetGeneration)
                    ]);
                } catch (error) {
                    if (targetDevice === currentDevice) setMessage(`Error: ${error.message}`);
                }
            });
            inputTabs.appendChild(button);
        });
    } catch (error) {
        if (generation !== selectionGeneration || selected !== currentDevice) return;
        hideInputs();
    }
}

function sendCommand(command) {
    if (!currentDevice) return;
    const selected = currentDevice;
    fetch("/api/command", {
        method:"POST", cache:"no-store",
        headers:{"Content-Type":"application/json"},
        body:JSON.stringify({device:selected, command})
    }).then(async response => {
        if (!response.ok) {
            const body = await response.json().catch(() => ({}));
            throw new Error(body.detail || `HTTP ${response.status}`);
        }
        if (selected === currentDevice) setMessage(command.replaceAll("_"," "));
    }).catch(error => {
        if (selected === currentDevice) setMessage(`Error: ${error.message}`);
    });
}

async function findRemote() {
    if (!findRemoteButton) return;

    findRemoteButton.disabled = true;
    setMessage("Finding remote...");

    try {
        await api("/api/remote/find", {
            method: "POST"
        });

        setMessage("Find Remote activated");
    } catch (error) {
        setMessage(`Error: ${error.message}`);
    } finally {
        findRemoteButton.disabled = false;
    }
}

async function connectDevice() {
    if (!currentDevice) return;
    const selected = currentDevice;
    const generation = selectionGeneration;
    try {
        setMessage("Connecting...");
        await api(`/api/devices/${encodeURIComponent(selected)}/connect`, {method:"POST"});
        if (selected !== currentDevice || generation !== selectionGeneration) return;
        await refreshSelectedStatus(generation);
        setMessage("Connected");
    } catch (error) {
        if (selected === currentDevice) setMessage(`Error: ${error.message}`);
    }
}

document.querySelectorAll("[data-command]").forEach(button => {
    button.addEventListener("click", () => sendCommand(button.dataset.command));
});
connectButton.addEventListener("click", connectDevice);
findRemoteButton.addEventListener("click", findRemote);
deviceSelect.addEventListener("change", () => selectDevice(deviceSelect.value));
setInterval(() => refreshSelectedStatus(), 5000);
loadDevices();
