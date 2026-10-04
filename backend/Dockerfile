FROM mcr.microsoft.com/playwright:v1.55.0-noble
WORKDIR /app
COPY package.json ./
RUN npm install --omit=dev
COPY server.js index.html ./
EXPOSE 32124/tcp
EXPOSE 32123/udp
CMD ["node","server.js"]
