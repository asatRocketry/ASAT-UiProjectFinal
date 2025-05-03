const express = require('express');
const path = require('path');
const Onvif = require('node-onvif');

const app = express();
const port = 3000;

app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json());

// Initialize the ONVIF device (update with your camera's details)
const device = new Onvif.OnvifDevice({
  xaddr: 'http://192.168.1.110:2020/onvif/device_service', // Change to your camera's ONVIF address
  user: 'camera1',    // Your camera's username
  pass: 'password'  // Your camera's password
});

device.init()
  .then(() => {
    console.log('ONVIF device initialized');
  })
  .catch(error => {
    console.error('Error initializing ONVIF device:', error);
  });

// Helper to get profile token
function getProfileToken() {
  const profile = device.getCurrentProfile();
  return profile && profile.token ? profile.token : null;
}

// Start continuous movement
app.post('/control/start', async (req, res) => {
  const { command, speed } = req.body;
  console.log(`Start command: ${command} with speed: ${speed}`);

  if (!device.services.ptz) {
    res.status(500).send('PTZ service not available on this device.');
    return;
  }

  const profileToken = getProfileToken();
  if (!profileToken || typeof profileToken !== 'string') {
    res.status(500).send('Invalid profile token.');
    return;
  }

  try {
    let velocity;
    switch (command) {
      case 'left':
        velocity = { x: -speed, y: 0, z: 0 };
        break;
      case 'right':
        velocity = { x: speed, y: 0, z: 0 };
        break;
      case 'up':
        velocity = { x: 0, y: speed, z: 0 };
        break;
      case 'down':
        velocity = { x: 0, y: -speed, z: 0 };
        break;
      default:
        res.status(400).send('Unknown command');
        return;
    }
    await device.services.ptz.continuousMove({
      ProfileToken: profileToken,
      Velocity: velocity
    });
    res.send('Movement started');
  } catch (error) {
    console.error('Error starting PTZ command:', error);
    res.status(500).send('Error executing command.');
  }
});

// Stop the movement
app.post('/control/stop', async (req, res) => {
  console.log('Stop command received');
  if (!device.services.ptz) {
    res.status(500).send('PTZ service not available on this device.');
    return;
  }

  const profileToken = getProfileToken();
  if (!profileToken || typeof profileToken !== 'string') {
    res.status(500).send('Invalid profile token.');
    return;
  }

  try {
    await device.services.ptz.stop({
      ProfileToken: profileToken,
      PanTilt: true,
      Zoom: true
    });
    res.send('Movement stopped');
  } catch (error) {
    console.error('Error stopping movement:', error);
    res.status(500).send('Error stopping movement.');
  }
});

app.listen(port, () => {
  console.log(`Server listening on port ${port}`);
});
