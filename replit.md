# AgenticAI CEP

## Project Overview
Agentic AI Complex Event Processing (CEP) platform. This project was imported from GitHub as an empty repository stub and set up in the Replit environment.

## Tech Stack
- **Runtime**: Node.js 20
- **Framework**: Express.js
- **Frontend**: Static HTML/CSS served from `/public`
- **Port**: 5000

## Project Structure
```
├── server.js        # Express web server (serves static files on port 5000)
├── public/
│   └── index.html   # Landing page
├── package.json     # Node.js dependencies
└── replit.md        # This file
```

## Running the App
The app runs via the "Start application" workflow:
```bash
node server.js
```
Starts an Express server on `0.0.0.0:5000`.

## Development Notes
- The server listens on `0.0.0.0` to support Replit's proxied preview iframe.
- Static assets are served from the `public/` directory.
- Add your application logic to `server.js` and frontend code to `public/`.
