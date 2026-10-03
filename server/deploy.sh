#!/bin/bash

# Load environment variables from .env file
if [ -f .env ]; then
    export $(grep -v '^#' .env | xargs)
else
    echo "Error: .env file not found"
    exit 1
fi

# Check if required environment variables are set
if [ -z "$ASSEMBLYAI_API_KEY" ] || [ -z "$OPENAI_API_KEY" ]; then
    echo "Error: ASSEMBLYAI_API_KEY and OPENAI_API_KEY must be set in .env file"
    exit 1
fi

# Deploy to Cloud Run using Docker
gcloud run deploy robot-server \
    --source . \
    --region europe-west1 \
    --project aha-robot \
    --allow-unauthenticated \
    --platform managed \
    --service-account robot-server@aha-robot.iam.gserviceaccount.com \
    --set-env-vars="ASSEMBLYAI_API_KEY=$ASSEMBLYAI_API_KEY,OPENAI_API_KEY=$OPENAI_API_KEY"

echo "Deployment complete!"