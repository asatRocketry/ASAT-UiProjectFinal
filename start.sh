#!/bin/bash

# Define absolute base directory (adjust if needed)
PROJECTS_DIR="/home/efidr/Desktop/ui-project"
SESSION="project_env"

cd "$PROJECTS_DIR" || {
  echo "❌ Failed to cd into $PROJECTS_DIR"
  exit 1
}

# Kill existing tmux session if any
tmux kill-session -t $SESSION 2>/dev/null

# Start a new session with the redis window
tmux new-session -d -s $SESSION -n redis

# Pane 1: Redis
tmux send-keys -t $SESSION:0 "cd $PROJECTS_DIR" C-m
tmux send-keys -t $SESSION:0 "sudo docker run -p 6379:6379 --rm redis/redis-stack-server:latest 2>&1 | tee docker-redis.log" C-m

# Give Redis a few seconds before launching everything else
sleep 5

# Pane 2: Ground Station
tmux new-window -t $SESSION:1 -n cbackend
tmux send-keys -t $SESSION:1 "cd $PROJECTS_DIR/cbackend && ./ground_station" C-m

# Pane 3: NGINX
tmux new-window -t $SESSION:2 -n nginx
tmux send-keys -t $SESSION:2 "cd $PROJECTS_DIR/nginx && sudo nginx -c $PROJECTS_DIR/nginx/nginx.conf -p $PROJECTS_DIR/nginx" C-m

# Pane 4: RTSP2WS
tmux new-window -t $SESSION:3 -n rtsp2ws
tmux send-keys -t $SESSION:3 "cd $PROJECTS_DIR/rtsp2ws-master && ./rtsp2ws_server rtsp://adminsat:asatisgaysat@192.168.88.31/stream1 8002" C-m

tmux new-window -t $SESSION:4 -n rtsp2ws
tmux send-keys -t $SESSION:4 "cd $PROJECTS_DIR/rtsp2ws-master && ./rtsp2ws_server rtsp://adminsat:asatisgaysat@192.168.88.32/stream1 8003" C-m

tmux new-window -t $SESSION:5 -n rtsp2ws
tmux send-keys -t $SESSION:5 "cd $PROJECTS_DIR/rtsp2ws-master && ./rtsp2ws_server rtsp://adminsat:asatisgaysat@192.168.88.33/stream1 8004" C-m

# Pane 5: Frontend
tmux new-window -t $SESSION:6 -n frontend
tmux send-keys -t $SESSION:6 "cd $PROJECTS_DIR/frontend && npm run dev" C-m

# Attach to the tmux session
tmux attach-session -t $SESSION
