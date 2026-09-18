class Solution {
public:
    int maxSubArray(vector<int> &nums) {
        int currS=0;
        int maxSum=INT_MIN;
        for(int i : nums){
            currS+=i;
            maxSum=max(currS,maxSum);
            if(currS<0)
                currS=0;
        }
        return maxSum;
    }
};